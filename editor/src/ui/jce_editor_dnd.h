/* jce_editor_dnd.h — Centralized ImGui drag-and-drop payload type IDs.
 *
 * ImGui's payload routing keys on a small string (max 32 chars).  All
 * editor panels / viewers / dialogs that produce or consume a given
 * payload must agree on the exact spelling — a typo in one spot
 * silently breaks drag-drop.  Centralize the canonical names here.
 *
 * Producers:  ImGui::SetDragDropPayload(JCE_DND_*, ...);
 * Consumers:  ImGui::AcceptDragDropPayload(JCE_DND_*);
 */
#pragma once

/* Payload: absolute path to a project asset (NUL-terminated string).
   Produced by Assets grid; consumed by inspectors, scene view,
   sequencer, animator, material viewer, terrain. */
#define JCE_DND_ASSET_PATH "JCE_ASSET_PATH"

/* Payload: scene entity ID (uint32_t).
   Produced by hierarchy node drag; consumed by hierarchy reparent. */
#define JCE_DND_ENTITY     "JCE_ENTITY"
