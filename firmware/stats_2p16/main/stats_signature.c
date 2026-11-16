#include "stats_signature.h"

#include <string.h>

static uint32_t hash_bytes(uint32_t hash, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    for (size_t index = 0; index < size; index++) {
        hash ^= bytes[index];
        hash *= 16777619;
    }
    return hash;
}

static uint32_t hash_string(uint32_t hash, const char *text)
{
    if (text) {
        hash = hash_bytes(hash, text, strlen(text));
    }
    return hash_bytes(hash, "\xff", 1);
}

static uint32_t hash_int(uint32_t hash, int value)
{
    return hash_bytes(hash, &value, sizeof(value));
}

uint32_t stats_content_crc(const cJSON *document)
{
    uint32_t hash = 2166136261u;
    const cJSON *cats = cJSON_GetObjectItem(document, "cats");
    int count = cJSON_GetArraySize(cats);
    hash = hash_int(hash, count);
    for (int index = 0; index < count && index < 4; index++) {
        const cJSON *cat = cJSON_GetArrayItem(cats, index);
        hash = hash_string(hash, cJSON_GetStringValue(cJSON_GetObjectItem(cat, "cat_id")));
        hash = hash_string(hash, cJSON_GetStringValue(cJSON_GetObjectItem(cat, "name")));
        hash = hash_int(hash, cJSON_GetNumberValue(cJSON_GetObjectItem(cat, "today_count")));
        const cJSON *box_counts = cJSON_GetObjectItem(cat, "box_counts");
        int box_total = cJSON_GetArraySize(box_counts);
        hash = hash_int(hash, box_total);
        for (int box = 0; box < box_total; box++) {
            const cJSON *entry = cJSON_GetArrayItem(box_counts, box);
            hash = hash_string(hash, cJSON_GetStringValue(cJSON_GetObjectItem(entry, "box_id")));
            hash = hash_int(hash, cJSON_GetNumberValue(cJSON_GetObjectItem(entry, "count")));
        }
    }
    return hash_int(hash, cJSON_GetNumberValue(cJSON_GetObjectItem(document, "unknown_count")));
}
