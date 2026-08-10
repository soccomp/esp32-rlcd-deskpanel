#pragma once

#include <stdint.h>

constexpr uint8_t CHORD_STRING_COUNT = 6;
constexpr uint8_t CHORD_AUDIO_NOTE_COUNT = 5;
constexpr uint8_t CHORD_DATA_COUNT = 12;
constexpr uint8_t CHORD_GROUP_COUNT = 2;

enum ChordGroup : uint8_t {
    CHORD_GROUP_OPEN = 0,
    CHORD_GROUP_SEVENTH = 1,
};

struct ChordData {
    const char *name;
    const char *quality;
    ChordGroup group;
    int8_t frets[CHORD_STRING_COUNT];       // low E -> high E; -1=mute, 0=open
    uint8_t fingers[CHORD_STRING_COUNT];    // 1=index ... 4=pinky; 0=no finger
    const char *string_notes[CHORD_STRING_COUNT];
    const char *notes;
    const char *formula;
    float audio_hz[CHORD_AUDIO_NOTE_COUNT]; // ascending Fender Clean arpeggio
};

const ChordData &chord_data_get(uint8_t index);
uint8_t chord_data_group_first(ChordGroup group);
uint8_t chord_data_group_size(ChordGroup group);
const char *chord_data_group_name(ChordGroup group);

