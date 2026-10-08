#include "miband_xiaomi_mass.h"
#include <stdlib.h>
#include <string.h>

struct MiBandXiaomiMassReceiver {
    uint16_t total_parts;
    uint16_t next_part;
    uint8_t *stream;
    size_t stream_length;
    size_t stream_capacity;
    MiBandXiaomiMass result;
    bool complete;
};

static uint16_t get16(const uint8_t *data)
{
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

MiBandXiaomiMassReceiver *miband_xiaomi_mass_receiver_new(void)
{
    MiBandXiaomiMassReceiver *receiver = calloc(1, sizeof(*receiver));
    if (receiver) receiver->next_part = 1;
    return receiver;
}

void miband_xiaomi_mass_receiver_free(MiBandXiaomiMassReceiver *receiver)
{
    if (receiver) {
        free(receiver->stream);
        free(receiver);
    }
}

void miband_xiaomi_mass_receiver_reset(MiBandXiaomiMassReceiver *receiver)
{
    receiver->total_parts = 0;
    receiver->next_part = 1;
    receiver->stream_length = 0;
    receiver->complete = false;
    memset(&receiver->result, 0, sizeof(receiver->result));
}

static bool reserve(MiBandXiaomiMassReceiver *receiver, size_t extra)
{
    size_t wanted = receiver->stream_length + extra;
    size_t capacity = receiver->stream_capacity ? receiver->stream_capacity : 4096;
    uint8_t *stream;

    if (wanted > 64 * 1024 * 1024) return false;
    while (capacity < wanted) capacity *= 2;
    if (capacity == receiver->stream_capacity) return true;
    stream = realloc(receiver->stream, capacity);
    if (!stream) return false;
    receiver->stream = stream;
    receiver->stream_capacity = capacity;
    return true;
}

bool miband_xiaomi_mass_receiver_push(MiBandXiaomiMassReceiver *receiver,
                                      const uint8_t *body, size_t length)
{
    uint16_t total, current;

    if (!receiver || !body || length < 4 || receiver->complete) return false;
    total = get16(body); current = get16(body + 2);
    if (!total || !current || current > total ||
        (receiver->total_parts && total != receiver->total_parts) ||
        current != receiver->next_part || !reserve(receiver, length - 4)) {
        return false;
    }
    if (!receiver->total_parts) receiver->total_parts = total;
    memcpy(receiver->stream + receiver->stream_length, body + 4, length - 4);
    receiver->stream_length += length - 4;
    receiver->next_part++;
    if (current != total) return true;
    if (!miband_xiaomi_mass_decode(receiver->stream, receiver->stream_length,
                                   &receiver->result) ||
        !miband_xiaomi_mass_verify(&receiver->result)) {
        receiver->stream_length = 0;
        receiver->next_part = 1;
        receiver->total_parts = 0;
        return false;
    }
    receiver->complete = true;
    return true;
}

bool miband_xiaomi_mass_receiver_complete(const MiBandXiaomiMassReceiver *receiver)
{
    return receiver && receiver->complete;
}

const MiBandXiaomiMass *miband_xiaomi_mass_receiver_result(
    const MiBandXiaomiMassReceiver *receiver)
{
    return receiver && receiver->complete ? &receiver->result : NULL;
}

const uint8_t *miband_xiaomi_mass_receiver_data(
    const MiBandXiaomiMassReceiver *receiver, size_t *length)
{
    if (length) {
        *length = receiver && receiver->complete ? receiver->stream_length : 0;
    }
    return receiver && receiver->complete ? receiver->stream : NULL;
}
