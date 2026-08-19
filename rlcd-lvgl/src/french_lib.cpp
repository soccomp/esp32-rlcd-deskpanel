/* 法语对话词库：内置 200 组（french_dialogues.h），无 SD 依赖。
 * 8-14 起替代原"SD 优先 + 内置 100 句单句"方案。
 */
#include "french_lib.h"
#include "french_dialogues.h"

uint16_t french_lib_count(void)
{
    return FRENCH_DIALOGUES_COUNT;
}

bool french_lib_get(uint16_t idx, french_dialogue_t *out)
{
    if (!out || idx >= FRENCH_DIALOGUES_COUNT) return false;
    *out = kFrenchDialogues[idx];
    return true;
}
