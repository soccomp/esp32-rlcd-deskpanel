#include "chord_data.h"

namespace {

const ChordData CHORDS[CHORD_DATA_COUNT] = {
    // Beginner open chords
    {"Em", "MINOR", CHORD_GROUP_OPEN,
     {0, 2, 2, 0, 0, 0}, {0, 2, 3, 0, 0, 0},
     {"E", "B", "E", "G", "B", "E"}, "E-G-B", "1-b3-5",
     {82.41f, 123.47f, 164.81f, 196.00f, 246.94f}},
    {"Am", "MINOR", CHORD_GROUP_OPEN,
     {-1, 0, 2, 2, 1, 0}, {0, 0, 2, 3, 1, 0},
     {"X", "A", "E", "A", "C", "E"}, "A-C-E", "1-b3-5",
     {110.00f, 164.81f, 220.00f, 261.63f, 329.63f}},
    {"C", "MAJOR", CHORD_GROUP_OPEN,
     {-1, 3, 2, 0, 1, 0}, {0, 3, 2, 0, 1, 0},
     {"X", "C", "E", "G", "C", "E"}, "C-E-G", "1-3-5",
     {130.81f, 164.81f, 196.00f, 261.63f, 329.63f}},
    {"G", "MAJOR", CHORD_GROUP_OPEN,
     {3, 2, 0, 0, 0, 3}, {2, 1, 0, 0, 0, 3},
     {"G", "B", "D", "G", "B", "G"}, "G-B-D", "1-3-5",
     {98.00f, 123.47f, 146.83f, 196.00f, 246.94f}},
    {"D", "MAJOR", CHORD_GROUP_OPEN,
     {-1, -1, 0, 2, 3, 2}, {0, 0, 0, 1, 3, 2},
     {"X", "X", "D", "A", "D", "F#"}, "D-F#-A", "1-3-5",
     {146.83f, 220.00f, 293.66f, 369.99f, 440.00f}},
    {"A", "MAJOR", CHORD_GROUP_OPEN,
     {-1, 0, 2, 2, 2, 0}, {0, 0, 1, 2, 3, 0},
     {"X", "A", "E", "A", "C#", "E"}, "A-C#-E", "1-3-5",
     {110.00f, 164.81f, 220.00f, 277.18f, 329.63f}},
    {"E", "MAJOR", CHORD_GROUP_OPEN,
     {0, 2, 2, 1, 0, 0}, {0, 2, 3, 1, 0, 0},
     {"E", "B", "E", "G#", "B", "E"}, "E-G#-B", "1-3-5",
     {82.41f, 123.47f, 164.81f, 207.65f, 246.94f}},
    {"Dm", "MINOR", CHORD_GROUP_OPEN,
     {-1, -1, 0, 2, 3, 1}, {0, 0, 0, 2, 3, 1},
     {"X", "X", "D", "A", "D", "F"}, "D-F-A", "1-b3-5",
     {146.83f, 220.00f, 293.66f, 349.23f, 440.00f}},

    // Seventh-chord progression
    {"Cmaj7", "MAJOR 7", CHORD_GROUP_SEVENTH,
     {-1, 3, 2, 0, 0, 0}, {0, 3, 2, 0, 0, 0},
     {"X", "C", "E", "G", "B", "E"}, "C-E-G-B", "1-3-5-7",
     {130.81f, 164.81f, 196.00f, 246.94f, 261.63f}},
    {"Am7", "MINOR 7", CHORD_GROUP_SEVENTH,
     {-1, 0, 2, 0, 1, 0}, {0, 0, 2, 0, 1, 0},
     {"X", "A", "E", "G", "C", "E"}, "A-C-E-G", "1-b3-5-b7",
     {110.00f, 130.81f, 164.81f, 196.00f, 246.94f}},
    {"Dm7", "MINOR 7", CHORD_GROUP_SEVENTH,
     {-1, -1, 0, 2, 1, 1}, {0, 0, 0, 2, 1, 1},
     {"X", "X", "D", "A", "C", "F"}, "D-F-A-C", "1-b3-5-b7",
     {146.83f, 174.61f, 220.00f, 261.63f, 329.63f}},
    {"G7", "DOM 7", CHORD_GROUP_SEVENTH,
     {3, 2, 0, 0, 0, 1}, {3, 2, 0, 0, 0, 1},
     {"G", "B", "D", "G", "B", "F"}, "G-B-D-F", "1-3-5-b7",
     {98.00f, 123.47f, 146.83f, 174.61f, 220.00f}},
};

} // namespace

const ChordData &chord_data_get(uint8_t index)
{
    return CHORDS[index < CHORD_DATA_COUNT ? index : 0];
}

uint8_t chord_data_group_first(ChordGroup group)
{
    return group == CHORD_GROUP_SEVENTH ? 8 : 0;
}

uint8_t chord_data_group_size(ChordGroup group)
{
    return group == CHORD_GROUP_SEVENTH ? 4 : 8;
}

const char *chord_data_group_name(ChordGroup group)
{
    return group == CHORD_GROUP_SEVENTH ? "7TH" : "OPEN";
}
