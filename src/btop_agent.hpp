/* Agent panel integration for btop++ fork */

#pragma once

#include <string>
#include <vector>

using std::string;
using std::vector;

namespace Agent {
	struct SessionInfo {
		string session_id;
		string agent_type;
		string time_str;
		string command;
		int pid = 0;
		bool running = false;
		bool waiting = false;
	};

	extern string box;
	extern int x, y, width, height, min_width, min_height;
	extern bool shown, redraw;
	extern bool focused;

	vector<SessionInfo> collect(bool no_update);
	string draw(const vector<SessionInfo>& sessions, bool force_redraw, bool no_update);
	bool register_click(const string& session_id);
	void attach_session(const string& session_id);
	void toggle_focus();
	bool handle_nav_key(const std::string_view key);
	bool activate_selected();
	bool resume_selected();
	bool kill_selected();
}
