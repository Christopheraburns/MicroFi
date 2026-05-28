// manifest.cpp -- builds the EFM-shaped agent manifest from the static
// processor registry.
//
// Shape (matches Apache NiFi MiNiFi / Cloudera EFM 2.x):
//   {
//     "identifier": "<sha256 hex of this json, populated after build>",
//     "agentType":  "cpp",
//     "version":    "0.1.0",
//     "buildInfo":  { ... },
//     "bundles": [
//       {
//         "group": "org.apache.nifi",
//         "artifact": "microfi-system",
//         "version": "0.1.0",
//         "componentManifest": {
//           "processors": [
//             {
//               "group": "org.apache.nifi",        // bundle coordinates repeated
//               "artifact": "microfi-system",       // on each processor entry --
//               "version": "0.1.0",                // EFM needs these to build a
//               "type": "<processor name>",         // unique component identity
//               "typeDescription": "<description>",
//               "inputRequirement": "INPUT_FORBIDDEN|INPUT_REQUIRED|INPUT_ALLOWED",
//               "isSingleThreaded": false,
//               "supportsDynamicProperties": false,
//               "supportsDynamicRelationships": false,
//               "supportedRelationships": [{"name":"success","description":"..."}]
//               // NOTE: propertyDescriptors omitted when empty -- EFM stores
//               // an empty object as "" which corrupts the stored manifest.
//             }, ...
//           ],
//           "controllerServices": [],
//           "reportingTasks": []
//         }
//       }
//     ],
//     "schedulingDefaults": { ... },
//     "supportedOperations": [ ... ]
//   }
//
// The "identifier" is the SHA-256 of the serialized JSON *without* the
// identifier field itself -- we build the JSON first, hash it, then
// patch the identifier in. The manifest is serialized once at boot and
// cached; on a microcontroller we don't pay this cost more than once.

#include "microfi/manifest.h"

#include "microfi/registry.h"

#include "cJSON.h"
#include "esp_log.h"
#include "psa/crypto.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace microfi {

namespace {

const char* TAG = "microfi.manifest";

constexpr const char* kAgentType         = "cpp";
constexpr const char* kVersion           = "0.1.0";
constexpr const char* kBundleGroup       = "org.apache.nifi";
constexpr const char* kBundleArtifact    = "microfi-system";

char*  s_manifest_json     = nullptr;   // heap-allocated, freed never
size_t s_manifest_json_len = 0;
char   s_manifest_hash[65] = {0};       // 64 hex + NUL
bool   s_initialised       = false;

void to_hex(const uint8_t* in, size_t in_len, char* out) {
    static const char* hex = "0123456789abcdef";
    for (size_t i = 0; i < in_len; ++i) {
        out[i*2]     = hex[(in[i] >> 4) & 0xf];
        out[i*2 + 1] = hex[in[i]        & 0xf];
    }
    out[in_len * 2] = '\0';
}

Status sha256_hex(const char* in, size_t in_len, char out_hex[65]) {
    uint8_t digest[32] = {0};
    size_t digest_len = 0;
    const psa_status_t st = psa_hash_compute(
        PSA_ALG_SHA_256,
        reinterpret_cast<const uint8_t*>(in),
        in_len,
        digest,
        sizeof(digest),
        &digest_len);
    if (st != PSA_SUCCESS || digest_len != sizeof(digest)) {
        return Status::Internal;
    }
    to_hex(digest, sizeof(digest), out_hex);
    return Status::Ok;
}

cJSON* build_scheduling_defaults() {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "defaultSchedulingStrategy", "TIMER_DRIVEN");
    cJSON_AddNumberToObject(o, "defaultSchedulingPeriodMillis", 1000);
    cJSON_AddNumberToObject(o, "defaultRunDurationNanos", 0);
    cJSON_AddNumberToObject(o, "defaultMaxConcurrentTasks", 1);
    cJSON_AddNumberToObject(o, "penalizationPeriodMillis", 30000);
    cJSON_AddNumberToObject(o, "yieldDurationMillis", 1000);
    return o;
}

cJSON* build_supported_operations() {
    cJSON* arr = cJSON_CreateArray();

    // HEARTBEAT, ACKNOWLEDGE, DESCRIBE -- plain, no sub-properties.
    static const char* plain_ops[] = { "HEARTBEAT", "ACKNOWLEDGE" };
    for (const char* op : plain_ops) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", op);
        cJSON_AddItemToObject(o, "properties", cJSON_CreateObject());
        cJSON_AddItemToArray(arr, o);
    }

    // DESCRIBE with sub-operands EFM recognises.
    {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "DESCRIBE");
        cJSON* props = cJSON_CreateObject();
        cJSON_AddItemToObject(props, "manifest",      cJSON_CreateObject());
        cJSON_AddItemToObject(props, "configuration", cJSON_CreateObject());
        cJSON_AddItemToObject(o, "properties", props);
        cJSON_AddItemToArray(arr, o);
    }

    // UPDATE with configuration sub-operand -- required for EFM Flow Designer
    // to offer flow management for this agent class.
    {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "type", "UPDATE");
        cJSON* props = cJSON_CreateObject();
        cJSON_AddItemToObject(props, "configuration", cJSON_CreateObject());
        cJSON_AddItemToObject(o, "properties", props);
        cJSON_AddItemToArray(arr, o);
    }

    return arr;
}

cJSON* build_processor_entry(const ProcessorDescriptor* d) {
    cJSON* p = cJSON_CreateObject();

    // Bundle coordinates must be repeated at the processor level so EFM can
    // build a unique component identity (group:artifact:version:type).
    // Without these, EFM stores null for all three and the Flow Designer
    // cannot resolve the processor in the palette.
    cJSON_AddStringToObject(p, "group",    kBundleGroup);
    cJSON_AddStringToObject(p, "artifact", kBundleArtifact);
    cJSON_AddStringToObject(p, "version",  kVersion);

    cJSON_AddStringToObject(p, "type", d->name);
    cJSON_AddStringToObject(p, "typeDescription",
                            d->description ? d->description : "");

    // inputRequirement tells the Flow Designer whether to draw an incoming
    // connection port on this processor's canvas node.
    const char* input_req = (d->input_requirement != nullptr)
                              ? d->input_requirement
                              : "INPUT_ALLOWED";
    cJSON_AddStringToObject(p, "inputRequirement",  input_req);
    cJSON_AddBoolToObject  (p, "isSingleThreaded",  false);

    cJSON_AddBoolToObject(p, "supportsDynamicProperties",    false);
    cJSON_AddBoolToObject(p, "supportsDynamicRelationships", false);

    cJSON* rels = cJSON_CreateArray();
    cJSON* success = cJSON_CreateObject();
    cJSON_AddStringToObject(success, "name", "success");
    cJSON_AddStringToObject(success, "description",
                            "FlowFiles produced by this processor.");
    cJSON_AddItemToArray(rels, success);
    cJSON_AddItemToObject(p, "supportedRelationships", rels);

    // propertyDescriptors: only emitted when the processor declares at least
    // one property. An empty object {} is deliberately avoided -- EFM's Java
    // deserializer silently re-serializes it as the string "", which corrupts
    // the stored manifest and prevents the processor from rendering in the
    // palette. Processors with no properties leave this key absent entirely.
    if (d->property_count > 0 && d->properties != nullptr) {
        cJSON* props = cJSON_CreateObject();
        for (size_t i = 0; i < d->property_count; ++i) {
            const PropertyDescriptor& pd = d->properties[i];
            cJSON* entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "name",
                                    pd.name ? pd.name : "");
            cJSON_AddStringToObject(entry, "description",
                                    pd.description ? pd.description : "");
            if (pd.default_value != nullptr) {
                cJSON_AddStringToObject(entry, "defaultValue", pd.default_value);
            }
            cJSON_AddBoolToObject(entry, "required", pd.required);

            if (pd.allowable_count > 0 && pd.allowable_values != nullptr) {
                cJSON* av_arr = cJSON_CreateArray();
                for (size_t j = 0; j < pd.allowable_count; ++j) {
                    const AllowableValue& av = pd.allowable_values[j];
                    const char* dn = (av.display_name != nullptr)
                                       ? av.display_name : av.value;
                    cJSON* av_entry = cJSON_CreateObject();
                    cJSON_AddStringToObject(av_entry, "value",       av.value);
                    cJSON_AddStringToObject(av_entry, "displayName", dn);
                    cJSON_AddItemToArray(av_arr, av_entry);
                }
                cJSON_AddItemToObject(entry, "allowableValues", av_arr);
            }

            cJSON_AddItemToObject(props, pd.name, entry);
        }
        cJSON_AddItemToObject(p, "propertyDescriptors", props);
    }

    return p;
}

cJSON* build_bundle(const Registry& reg) {
    cJSON* bundle = cJSON_CreateObject();
    cJSON_AddStringToObject(bundle, "group",    kBundleGroup);
    cJSON_AddStringToObject(bundle, "artifact", kBundleArtifact);
    cJSON_AddStringToObject(bundle, "version",  kVersion);

    cJSON* cm = cJSON_CreateObject();
    cJSON* processors = cJSON_CreateArray();
    for (size_t i = 0; i < reg.count(); ++i) {
        cJSON_AddItemToArray(processors, build_processor_entry(reg.at(i)));
    }
    cJSON_AddItemToObject(cm, "processors",        processors);
    cJSON_AddItemToObject(cm, "controllerServices", cJSON_CreateArray());
    cJSON_AddItemToObject(cm, "reportingTasks",     cJSON_CreateArray());
    cJSON_AddItemToObject(bundle, "componentManifest", cm);
    return bundle;
}

cJSON* build_build_info() {
    cJSON* b = cJSON_CreateObject();
    cJSON_AddStringToObject(b, "version",   kVersion);
    cJSON_AddStringToObject(b, "revision",  "dev");
    cJSON_AddNumberToObject(b, "timestamp", 0);
    cJSON_AddStringToObject(b, "targetArch","esp32");
    cJSON_AddStringToObject(b, "compiler",  "gcc");
    cJSON_AddStringToObject(b, "compilerFlags",
                            "-fno-exceptions -fno-rtti -std=gnu++17");
    return b;
}

}  // namespace

Status manifest_init() {
    if (s_initialised) return Status::Ok;

    auto& reg = Registry::instance();

    cJSON* root = cJSON_CreateObject();
    // Placeholder identifier is a 64-character all-zero hex string. cJSON's
    // SetValuestring won't grow a value's allocation, so the placeholder has
    // to be at least as long as the real SHA-256 hex (64 chars) that we
    // patch in below.
    cJSON_AddStringToObject(root, "identifier",
        "0000000000000000000000000000000000000000000000000000000000000000");
    cJSON_AddStringToObject(root, "agentType",  kAgentType);
    cJSON_AddStringToObject(root, "version",    kVersion);
    cJSON_AddItemToObject  (root, "buildInfo",  build_build_info());

    cJSON* bundles = cJSON_CreateArray();
    cJSON_AddItemToArray(bundles, build_bundle(reg));
    cJSON_AddItemToObject(root, "bundles", bundles);

    cJSON_AddItemToObject(root, "schedulingDefaults",  build_scheduling_defaults());
    cJSON_AddItemToObject(root, "supportedOperations", build_supported_operations());

    char* draft = cJSON_PrintUnformatted(root);
    if (draft == nullptr) {
        cJSON_Delete(root);
        ESP_LOGE(TAG, "cJSON_PrintUnformatted failed (draft)");
        return Status::OutOfMemory;
    }

    // Hash the *draft* (which has identifier="") and use that as the
    // manifest identifier. This is the same convention nifi-minifi-cpp
    // uses: the manifest id is the hash of its own serialized form.
    if (sha256_hex(draft, std::strlen(draft), s_manifest_hash) != Status::Ok) {
        cJSON_free(draft);
        cJSON_Delete(root);
        ESP_LOGE(TAG, "sha256 failed");
        return Status::Internal;
    }
    cJSON_free(draft);

    // Patch identifier in-place and re-serialize.
    cJSON* id_node = cJSON_GetObjectItem(root, "identifier");
    if (id_node == nullptr || !cJSON_IsString(id_node)) {
        cJSON_Delete(root);
        return Status::Internal;
    }
    cJSON_SetValuestring(id_node, s_manifest_hash);

    char* final_json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (final_json == nullptr) {
        ESP_LOGE(TAG, "cJSON_PrintUnformatted failed (final)");
        return Status::OutOfMemory;
    }

    s_manifest_json     = final_json;        // owned for the lifetime of the program
    s_manifest_json_len = std::strlen(final_json);
    s_initialised       = true;

    ESP_LOGI(TAG, "manifest built: %u bytes, hash=%s",
             static_cast<unsigned>(s_manifest_json_len), s_manifest_hash);
    ESP_LOGI(TAG, "manifest agentType check: %.200s", s_manifest_json);
    return Status::Ok;
}

const char* manifest_json()     { return s_manifest_json; }
size_t      manifest_json_len() { return s_manifest_json_len; }
const char* manifest_hash()     { return s_manifest_hash; }

}  // namespace microfi
                                                                                                                                                     