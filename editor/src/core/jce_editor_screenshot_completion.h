#ifndef JCE_EDITOR_SCREENSHOT_COMPLETION_H
#define JCE_EDITOR_SCREENSHOT_COMPLETION_H

#include <cstring>

/* Main-thread ownership of one backbuffer request; SDK completion is not consumed. */
struct JceEditorScreenshotOwner {
    bool active;
    bool interactive;
    unsigned source;
    char path[1100];
};

/* One renderer file slot is shared by backbuffer and texture capture. A
 * terminal legacy result stays leased until its submitting viewport consumes it. */
struct JceEditorScreenshotSlot { unsigned owner; };

inline bool jce_editor_screenshot_poll_game(const JceEditorScreenshotSlot &slot,
                                           bool scheduled)
{
    return slot.owner == 3 || scheduled;
}

template<class Submit>
bool jce_editor_screenshot_slot_submit(JceEditorScreenshotSlot &slot,
    unsigned owner, Submit submit)
{
    if (!owner || slot.owner) return false;
    if (!submit()) return false;
    slot.owner = owner;
    return true;
}

template<class Poll>
int jce_editor_screenshot_slot_poll(JceEditorScreenshotSlot &slot,
    unsigned owner, Poll poll)
{
    if (!owner || slot.owner != owner) return -1;
    const int result = poll();
    if (result == 1 || result == 2) slot.owner = 0;
    return result;
}

/* The legacy poll also pumps continuous recording. Its result is not a
 * backbuffer completion and must not release an unrelated file owner. */
template<class Poll>
int jce_editor_screenshot_legacy_poll(JceEditorScreenshotSlot &slot,
    unsigned owner, Poll poll)
{
    if (slot.owner == 0 || slot.owner == 1) {
        (void)poll();
        return -1;
    }
    return jce_editor_screenshot_slot_poll(slot, owner, poll);
}

template<class Submit>
bool jce_editor_screenshot_request(JceEditorScreenshotOwner &owner,
    const char *path, bool interactive, unsigned source, Submit submit)
{
    if (owner.active || !path || !path[0]) return false;
    const size_t length = std::strlen(path);
    if (length >= sizeof(owner.path) || !submit(path)) return false;
    std::memcpy(owner.path, path, length + 1);
    owner.interactive = interactive;
    owner.source = source;
    owner.active = true;
    return true;
}

template<class Poll, class Notify>
void jce_editor_screenshot_complete(JceEditorScreenshotOwner &owner,
    Poll poll, Notify notify)
{
    if (!owner.active) return;
    const int result = poll();
    if (result != 1 && result != 2) return;
    const JceEditorScreenshotOwner completed = owner;
    owner.active = false;
    notify(completed, result);
}

#endif
