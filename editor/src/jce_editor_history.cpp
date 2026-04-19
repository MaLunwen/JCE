/*
 * jce_editor_history.cpp  Undo/redo history and transaction management.
 *
 * Implements snapshot capture, history push/restore, and the public
 * undo/redo/batch-edit/transaction API.
 */

#include "jce_editor_state_internal.h"

/* ── History begin/end edit ───────────────────────────────────────── */

bool history_begin_edit(void)
{
	if (s_history_suspend_depth > 0)
		return false;

	if (s_history_edit_nesting == 0)
		s_history_outer_edit_pushed_snapshot = history_push_undo_snapshot();

	++s_history_edit_nesting;
	return true;
}

void history_end_edit(bool active)
{
	if (!active)
		return;
	if (s_history_edit_nesting <= 0)
		return;

	--s_history_edit_nesting;
	if (s_history_edit_nesting != 0)
		return;

	bool changed = true;
	EditorHistorySnapshot current;
	if (history_capture_snapshot(&current) && !s_undo_history.empty()) {
		const EditorHistorySnapshot &before = s_undo_history.back();
		changed = !(before.scene_json == current.scene_json
		            && before.scene_path == current.scene_path);
	}

	if (changed) {
		s_redo_history.clear();
		s.scene_modified = true;
	} else if (s_history_outer_edit_pushed_snapshot && !s_undo_history.empty()) {
		s_undo_history.pop_back();
	}

	s_history_outer_edit_pushed_snapshot = false;
}

/* ── History snapshot capture ────────────────────────────────────── */

bool history_capture_snapshot(EditorHistorySnapshot *out)
{
	if (!out)
		return false;

	cJSON *root = build_scene_json_root();
	if (!root)
		return false;

	char *json_text = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (!json_text)
		return false;

	out->scene_json = json_text;
	out->scene_path = s.current_scene_path;
	cJSON_free(json_text);
	return true;
}

bool history_push_undo_snapshot(void)
{
	if (s_history_suspend_depth > 0)
		return false;

	EditorHistorySnapshot snap;
	if (!history_capture_snapshot(&snap))
		return false;

	if (!s_undo_history.empty()) {
		const EditorHistorySnapshot &last = s_undo_history.back();
		if (last.scene_json == snap.scene_json && last.scene_path == snap.scene_path)
			return false;
	}

	if ((int)s_undo_history.size() >= JCE_UNDO_HISTORY_LIMIT)
		s_undo_history.erase(s_undo_history.begin());

	s_undo_history.push_back(std::move(snap));
	return true;
}

bool history_restore_snapshot(const EditorHistorySnapshot &snapshot,
                              const char *reason)
{
	if (snapshot.scene_json.empty())
		return false;

	cJSON *root = cJSON_Parse(snapshot.scene_json.c_str());
	if (!root) {
		LOG_WARN(LOG_TAG, "history restore parse failed (%s)",
		         reason ? reason : "unknown");
		return false;
	}

	HistorySuspendScope suspend;
	const char *label = snapshot.scene_path.empty()
		? (reason ? reason : "history")
		: snapshot.scene_path.c_str();
	bool ok = load_scene_from_parsed_root(root, label, snapshot.scene_path.c_str());
	cJSON_Delete(root);
	return ok;
}

/* ── Undo / Redo ─────────────────────────────────────────────────── */

void jce_state_undo(void)
{
	if (s_undo_history.empty()) {
		LOG_INFO(LOG_TAG, "undo: history empty");
		return;
	}

	EditorHistorySnapshot current;
	if (!history_capture_snapshot(&current)) {
		LOG_WARN(LOG_TAG, "undo: failed to capture current state");
		return;
	}

	EditorHistorySnapshot target = s_undo_history.back();
	s_undo_history.pop_back();

	if (!history_restore_snapshot(target, "undo")) {
		s_undo_history.push_back(std::move(target));
		LOG_WARN(LOG_TAG, "undo: restore failed");
		return;
	}

	if ((int)s_redo_history.size() >= JCE_UNDO_HISTORY_LIMIT)
		s_redo_history.erase(s_redo_history.begin());
	s_redo_history.push_back(std::move(current));

	LOG_INFO(LOG_TAG, "undo: applied");
}

void jce_state_redo(void)
{
	if (s_redo_history.empty()) {
		LOG_INFO(LOG_TAG, "redo: history empty");
		return;
	}

	EditorHistorySnapshot current;
	if (!history_capture_snapshot(&current)) {
		LOG_WARN(LOG_TAG, "redo: failed to capture current state");
		return;
	}

	EditorHistorySnapshot target = s_redo_history.back();
	s_redo_history.pop_back();

	if (!history_restore_snapshot(target, "redo")) {
		s_redo_history.push_back(std::move(target));
		LOG_WARN(LOG_TAG, "redo: restore failed");
		return;
	}

	if ((int)s_undo_history.size() >= JCE_UNDO_HISTORY_LIMIT)
		s_undo_history.erase(s_undo_history.begin());
	s_undo_history.push_back(std::move(current));

	LOG_INFO(LOG_TAG, "redo: applied");
}

bool jce_state_can_undo(void)
{
	return !s_undo_history.empty();
}

bool jce_state_can_redo(void)
{
	return !s_redo_history.empty();
}

/* ── Batch edit scope ────────────────────────────────────────────── */

void jce_state_begin_batch_edit(void)
{
	if (history_begin_edit())
		++s_history_manual_batch_depth;
}

void jce_state_end_batch_edit(void)
{
	if (s_history_manual_batch_depth <= 0)
		return;

	--s_history_manual_batch_depth;
	history_end_edit(true);
}

void jce_state_begin_transient_edit(void)
{
	++s_history_suspend_depth;
	++s_history_transient_batch_depth;
}

void jce_state_end_transient_edit(void)
{
	if (s_history_transient_batch_depth <= 0)
		return;

	--s_history_transient_batch_depth;
	if (s_history_suspend_depth > 0)
		--s_history_suspend_depth;

	if (s_history_transient_batch_depth == 0) {
		s_redo_history.clear();
		s.scene_modified = true;
	}
}

/* ── Explicit transaction scope ──────────────────────────────────── */

bool jce_state_begin_transaction(const char *label)
{
	if (s_transaction.active)
		return false;

	if (!history_capture_snapshot(&s_transaction.before))
		return false;

	if (label && label[0] != '\0')
		snprintf(s_transaction.label, sizeof(s_transaction.label), "%s", label);
	else
		snprintf(s_transaction.label, sizeof(s_transaction.label), "transaction");

	int depth_before = s_history_manual_batch_depth;
	jce_state_begin_batch_edit();
	if (s_history_manual_batch_depth == depth_before) {
		s_transaction.before.scene_json.clear();
		s_transaction.before.scene_path.clear();
		s_transaction.label[0] = '\0';
		return false;
	}

	s_transaction.active = true;
	return true;
}

void jce_state_commit_transaction(void)
{
	if (!s_transaction.active)
		return;

	jce_state_end_batch_edit();
	s_transaction.active = false;
	s_transaction.before.scene_json.clear();
	s_transaction.before.scene_path.clear();
	s_transaction.label[0] = '\0';
}

void jce_state_cancel_transaction(void)
{
	if (!s_transaction.active)
		return;

	history_restore_snapshot(s_transaction.before,
	                         s_transaction.label[0] ? s_transaction.label : "transaction");
	jce_state_end_batch_edit();

	s_transaction.active = false;
	s_transaction.before.scene_json.clear();
	s_transaction.before.scene_path.clear();
	s_transaction.label[0] = '\0';
}

bool jce_state_transaction_active(void)
{
	return s_transaction.active;
}

