#include "rws_odm.hpp"
#include "rws_xml.hpp"

#include "duckdb/common/exception.hpp"

namespace duckdb {

namespace {

//! RWS study OIDs follow "PROJECT(ENVIRONMENT)". The protocol name from
//! GlobalVariables is authoritative for the project; the environment can only
//! come from the OID suffix, so it stays empty when the suffix is absent.
void DeriveProjectAndEnvironment(RWSStudy &study) {
	string oid_project;
	string oid_environment;
	auto has_suffix = RWSSplitStudyOID(study.study_oid, oid_project, oid_environment);
	study.environment = has_suffix ? oid_environment : string();
	study.project_name = study.protocol_name.empty() ? oid_project : study.protocol_name;
}

void ExpectOdmRoot(XmlReader &reader, const char *what) {
	if (!reader.Read()) {
		throw IOException("rws: empty response where %s was expected", what);
	}
	if (!reader.IsElement(ODM_NS, "ODM")) {
		throw IOException("rws: response is not an ODM document where %s was expected", what);
	}
}

//! Rave writes booleans as Yes/No. Anything else is left unknown rather than
//! silently folded into false.
void ParseYesNo(const string &raw, bool &known, bool &value) {
	if (raw == "Yes") {
		known = true;
		value = true;
	} else if (raw == "No") {
		known = true;
		value = false;
	}
}

} // namespace

vector<RWSStudy> RWSParseStudies(const string &xml) {
	XmlReader reader(xml.c_str(), xml.size(), "the study list");
	ExpectOdmRoot(reader, "a study list");

	vector<RWSStudy> studies;
	RWSStudy current;
	bool in_study = false;

	while (reader.Read()) {
		if (reader.GetEvent() == XmlReader::Event::START_ELEMENT) {
			if (reader.NamespaceUri() != ODM_NS) {
				reader.SkipElement();
				continue;
			}
			const auto &name = reader.LocalName();
			if (name == "Study" && reader.Depth() == 2) {
				current = RWSStudy();
				current.study_oid = reader.GetAttribute("", "OID");
				in_study = true;
			} else if (in_study && name == "StudyName") {
				current.study_name = reader.ReadTextContent();
			} else if (in_study && name == "StudyDescription") {
				current.study_description = reader.ReadTextContent();
			} else if (in_study && name == "ProtocolName") {
				current.protocol_name = reader.ReadTextContent();
			}
		} else if (reader.GetEvent() == XmlReader::Event::END_ELEMENT && reader.NamespaceUri() == ODM_NS &&
		           reader.LocalName() == "Study" && reader.Depth() == 2) {
			DeriveProjectAndEnvironment(current);
			studies.push_back(current);
			in_study = false;
		}
	}
	return studies;
}

vector<RWSSubject> RWSParseSubjects(const string &xml) {
	XmlReader reader(xml.c_str(), xml.size(), "the subject list");
	ExpectOdmRoot(reader, "a subject list");

	vector<RWSSubject> subjects;
	string study_oid;
	string metadata_version_oid;
	bool in_subject = false;
	RWSSubject current;

	while (reader.Read()) {
		if (reader.GetEvent() == XmlReader::Event::END_ELEMENT) {
			if (in_subject && reader.NamespaceUri() == ODM_NS && reader.LocalName() == "SubjectData") {
				subjects.push_back(std::move(current));
				current = RWSSubject();
				in_subject = false;
			}
			continue;
		}
		if (reader.NamespaceUri() == ODM_NS) {
			const auto &name = reader.LocalName();
			if (name == "ClinicalData") {
				study_oid = reader.GetAttribute("", "StudyOID");
				metadata_version_oid = reader.GetAttribute("", "MetaDataVersionOID");
			} else if (name == "SubjectData") {
				current = RWSSubject();
				current.study_oid = study_oid;
				current.metadata_version_oid = metadata_version_oid;
				current.subject_key = reader.GetAttribute("", "SubjectKey");
				current.transaction_type = reader.GetAttribute("", "TransactionType");
				current.subject_key_type = reader.GetAttribute(MDSOL_NS, "SubjectKeyType");
				current.subject_name = reader.GetAttribute(MDSOL_NS, "SubjectName");
				ParseYesNo(reader.GetAttribute(MDSOL_NS, "SubjectActive"), current.active_known, current.active);
				ParseYesNo(reader.GetAttribute(MDSOL_NS, "Deleted"), current.deleted_known, current.deleted);
				for (auto &attribute : reader.Attributes()) {
					if (attribute.uri == MDSOL_NS) {
						current.attributes.emplace_back(attribute.local_name, attribute.value);
					}
				}
				// A SubjectName is only implied by the key when the server says
				// the key *is* the name; never infer it otherwise.
				if (current.subject_name.empty() && current.subject_key_type == "SubjectName") {
					current.subject_name = current.subject_key;
				}
				in_subject = true;
			} else if (in_subject && name == "SiteRef") {
				current.site_oid = reader.GetAttribute("", "LocationOID");
				current.site_number = reader.GetAttribute(MDSOL_NS, "StudyEnvSiteNumber");
			}
		} else if (in_subject && reader.NamespaceUri() == MDSOL_NS && reader.LocalName() == "Link") {
			auto href = reader.GetAttribute("http://www.w3.org/1999/xlink", "href");
			if (!href.empty()) {
				current.links.push_back(href);
			}
		}
	}
	return subjects;
}

shared_ptr<RWSDataset> RWSParseDataset(const string &xml, const string &dataset_type, const string &cv_last_updated) {
	XmlReader reader(xml.c_str(), xml.size(), "the clinical dataset");
	ExpectOdmRoot(reader, "a clinical dataset");

	auto dataset = make_shared_ptr<RWSDataset>();
	dataset->dataset_type = dataset_type;
	dataset->cv_last_updated = cv_last_updated;
	dataset->fetched_at = Timestamp::GetCurrentTimestamp();

	// Context carried down the ClinicalData/SubjectData/StudyEventData/FormData
	// spine and copied onto every ItemGroupData instance.
	string study_oid;
	string metadata_version_oid;
	string subject_key;
	string subject_transaction_type;
	string site_oid;
	string study_event_oid;
	string study_event_repeat_key;
	string study_event_transaction_type;
	string form_oid;
	string form_repeat_key;
	string form_transaction_type;

	RWSRecord record;
	bool in_record = false;

	auto note_once = [](vector<string> &seen, const string &value) {
		if (value.empty()) {
			return;
		}
		for (auto &existing : seen) {
			if (existing == value) {
				return;
			}
		}
		seen.push_back(value);
	};

	while (reader.Read()) {
		if (reader.GetEvent() == XmlReader::Event::END_ELEMENT) {
			if (in_record && reader.NamespaceUri() == ODM_NS && reader.LocalName() == "ItemGroupData") {
				dataset->records.push_back(std::move(record));
				record = RWSRecord();
				in_record = false;
			}
			continue;
		}
		if (reader.NamespaceUri() != ODM_NS) {
			continue;
		}
		const auto &name = reader.LocalName();
		if (name == "ClinicalData") {
			study_oid = reader.GetAttribute("", "StudyOID");
			metadata_version_oid = reader.GetAttribute("", "MetaDataVersionOID");
			if (dataset->study_oid.empty()) {
				dataset->study_oid = study_oid;
			}
		} else if (name == "SubjectData") {
			subject_key = reader.GetAttribute("", "SubjectKey");
			subject_transaction_type = reader.GetAttribute("", "TransactionType");
			site_oid.clear();
		} else if (name == "SiteRef") {
			site_oid = reader.GetAttribute("", "LocationOID");
			note_once(dataset->site_oids, site_oid);
		} else if (name == "StudyEventData") {
			study_event_oid = reader.GetAttribute("", "StudyEventOID");
			study_event_repeat_key = reader.GetAttribute("", "StudyEventRepeatKey");
			study_event_transaction_type = reader.GetAttribute("", "TransactionType");
			note_once(dataset->study_event_oids, study_event_oid);
		} else if (name == "FormData") {
			form_oid = reader.GetAttribute("", "FormOID");
			form_repeat_key = reader.GetAttribute("", "FormRepeatKey");
			form_transaction_type = reader.GetAttribute("", "TransactionType");
		} else if (name == "ItemGroupData") {
			record = RWSRecord();
			record.study_oid = study_oid;
			record.metadata_version_oid = metadata_version_oid;
			record.subject_key = subject_key;
			record.site_oid = site_oid;
			record.study_event_oid = study_event_oid;
			record.study_event_repeat_key = study_event_repeat_key;
			record.form_oid = form_oid;
			record.form_repeat_key = form_repeat_key;
			record.item_group_oid = reader.GetAttribute("", "ItemGroupOID");
			record.item_group_repeat_key = reader.GetAttribute("", "ItemGroupRepeatKey");
			record.subject_transaction_type = subject_transaction_type;
			record.study_event_transaction_type = study_event_transaction_type;
			record.form_transaction_type = form_transaction_type;
			record.item_group_transaction_type = reader.GetAttribute("", "TransactionType");
			in_record = true;
		} else if (name == "ItemData" && in_record) {
			RWSItemValue item;
			item.item_oid = reader.GetAttribute("", "ItemOID");
			item.has_value = reader.TryGetAttribute("", "Value", item.value);
			string is_null;
			if (reader.TryGetAttribute("", "IsNull", is_null)) {
				item.is_null_known = true;
				item.is_null = is_null == "Yes";
			}
			item.transaction_type = reader.GetAttribute("", "TransactionType");
			record.items.push_back(std::move(item));
		}
	}

	if (in_record) {
		throw IOException("rws: clinical dataset ended inside an ItemGroupData element");
	}

	// Derive the per-form column shape from the data: this deployment exposes
	// no metadata endpoint, so first appearance order is the only stable order
	// available. Every item OID seen for a form becomes a column of that form.
	for (idx_t record_index = 0; record_index < dataset->records.size(); record_index++) {
		auto &current = dataset->records[record_index];
		auto entry = dataset->forms.find(current.form_oid);
		if (entry == dataset->forms.end()) {
			RWSFormShape shape;
			shape.form_oid = current.form_oid;
			entry = dataset->forms.emplace(current.form_oid, std::move(shape)).first;
			dataset->form_oids.push_back(current.form_oid);
		}
		auto &shape = entry->second;
		shape.record_indexes.push_back(record_index);
		note_once(shape.item_group_oids, current.item_group_oid);
		for (auto &item : current.items) {
			if (shape.item_index.find(item.item_oid) == shape.item_index.end()) {
				shape.item_index[item.item_oid] = shape.item_oids.size();
				shape.item_oids.push_back(item.item_oid);
			}
		}
	}
	return dataset;
}

} // namespace duckdb
