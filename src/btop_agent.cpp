/* Agent panel integration for btop++ fork */

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <regex>
#include <unordered_set>
#include <filesystem>
#include <cstdlib>

#include <fmt/format.h>

#include "btop_agent.hpp"
#include "btop_config.hpp"
#include "btop_draw.hpp"
#include "btop_input.hpp"
#include "btop_theme.hpp"
#include "btop_tools.hpp"
#include "btop_shared.hpp"

using std::max;
using std::min;
using std::string;
using std::unordered_map;
using std::vector;

namespace Agent {
	string box;
	int x = 0;
	int y = 0;
	int width = 0;
	int height = 0;
	int min_width = 32;
	int min_height = 6;
	bool shown = false;
	bool redraw = true;
	bool focused = false;
	static uint64_t last_click_time = 0;
	static string last_click_session;
	static vector<SessionInfo> last_sessions;
	static int selected_index = 0;
	static int scroll_offset = 0;
	static int last_visible_rows = 0;

	static string expand_home(const string& path) {
		if (path.empty() || path[0] != '~')
			return path;
		const char* home = getenv("HOME");
		if (home == nullptr)
			return path;
		if (path.size() == 1)
			return string{home};
		if (path[1] == '/')
			return string{home} + path.substr(1);
		return path;
	}

	static bool match_agent_type(const string& text, string& agent_type) {
		auto lower = text;
		std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return std::tolower(c); });
		if (lower.find("claude") != string::npos) {
			agent_type = "claude";
			return true;
		}
		if (lower.find("codex") != string::npos) {
			agent_type = "codex";
			return true;
		}
		if (lower.find("gemini") != string::npos) {
			agent_type = "gemini";
			return true;
		}
		return false;
	}

	static string format_time(const string& timestamp) {
		auto t_pos = timestamp.find('T');
		if (t_pos != string::npos && timestamp.size() >= t_pos + 9) {
			return timestamp.substr(t_pos + 1, 8);
		}
		if (timestamp.size() >= 8)
			return timestamp.substr(timestamp.size() - 8);
		return timestamp;
	}

	static string extract_agent_type(const string& session_id) {
		const string prefix = "agent-";
		if (!session_id.starts_with(prefix))
			return "unknown";
		auto rest = session_id.substr(prefix.size());
		auto dash = rest.find('-');
		if (dash == string::npos)
			return rest;
		return rest.substr(0, dash);
	}

	struct TmuxInfo {
		int pid = 0;
		string last_output;
	};

	static bool is_waiting_for_input(const string& output) {
		static const std::vector<std::regex> patterns = {
			std::regex(R"(\?\s*$)"),
			std::regex(R"([Yy]/[Nn]\s*$)"),
			std::regex(R"([Pp]roceed\s*\?)"),
			std::regex(R"([Cc]ontinue\s*\?)"),
			std::regex(R"(Enter\s)"),
			std::regex(R"(Input\s*:)"),
			std::regex(R"(>\s*$)"),
			std::regex(R"(\$\s*$)")
		};

		for (const auto& pattern : patterns) {
			if (std::regex_search(output, pattern)) {
				return true;
			}
		}
		return false;
	}

	static unordered_map<string, TmuxInfo> tmux_sessions() {
		unordered_map<string, TmuxInfo> sessions;
		string output = Tools::exec_command("tmux list-sessions -F '#{session_name}:#{pane_pid}' 2>/dev/null");
		std::istringstream iss(output);
		string line;
		while (std::getline(iss, line)) {
			auto sep = line.find(':');
			if (sep == string::npos)
				continue;
			string name = line.substr(0, sep);
			string pid_str = line.substr(sep + 1);
			if (name.empty() || pid_str.empty())
				continue;
			try {
				int pid = std::stoi(pid_str);
				sessions[name].pid = pid;
				string capture_cmd = fmt::format("tmux capture-pane -t '{}' -p -J -S -10 2>/dev/null", name);
				sessions[name].last_output = Tools::exec_command(capture_cmd);
			} catch (...) {
				continue;
			}
		}
		return sessions;
	}

	static vector<SessionInfo> scan_processes(const std::unordered_set<int>& known_pids) {
		vector<SessionInfo> results;
		std::error_code ec;
		for (const auto& entry : std::filesystem::directory_iterator("/proc", ec)) {
			if (ec) break;
			if (!entry.is_directory()) continue;
			auto name = entry.path().filename().string();
			if (name.empty() || !std::all_of(name.begin(), name.end(), ::isdigit)) continue;

			int pid = 0;
			try {
				pid = std::stoi(name);
			} catch (...) {
				continue;
			}
			if (known_pids.contains(pid)) continue;

			string cmdline = Tools::readfile(entry.path() / "cmdline", "");
			string comm = Tools::readfile(entry.path() / "comm", "");

			for (auto& ch : cmdline) {
				if (ch == '\0') ch = ' ';
			}

			if (cmdline.find("btop-agent") != string::npos) continue;

			string agent_type;
			if (!match_agent_type(cmdline, agent_type) && !match_agent_type(comm, agent_type)) {
				continue;
			}

			SessionInfo info;
			info.session_id = fmt::format("proc-{}", pid);
			info.agent_type = agent_type;
			info.time_str = "live";
			info.command = cmdline.empty() ? comm : cmdline;
			info.pid = pid;
			info.running = true;
			info.waiting = false;
			results.push_back(std::move(info));
		}

		return results;
	}

	vector<SessionInfo> collect(bool no_update) {
		static vector<SessionInfo> cached;
		if (no_update && !cached.empty())
			return cached;

		vector<SessionInfo> sessions;
		const char* log_env = getenv("AGENT_SESSIONS_LOG");
		string log_path = log_env ? log_env : "~/.agent_sessions.log";
		log_path = expand_home(log_path);

		std::ifstream file(log_path);
		if (!file.good()) {
			cached.clear();
			return cached;
		}

		auto tmux_map = tmux_sessions();
		std::unordered_set<int> known_pids;
		string line;
		while (std::getline(file, line)) {
			auto first = line.find(" :: ");
			if (first == string::npos)
				continue;
			auto second = line.find(" :: ", first + 4);
			if (second == string::npos)
				continue;

			string timestamp = line.substr(0, first);
			string session_id = line.substr(first + 4, second - first - 4);
			string command = line.substr(second + 4);

			SessionInfo info;
			info.session_id = session_id;
			info.agent_type = extract_agent_type(session_id);
			info.time_str = format_time(timestamp);
			info.command = command;

			auto it = tmux_map.find(session_id);
			if (it != tmux_map.end()) {
				info.running = true;
				info.pid = it->second.pid;
				known_pids.insert(info.pid);
				info.waiting = is_waiting_for_input(it->second.last_output);
			}

			sessions.push_back(std::move(info));
		}

		auto proc_sessions = scan_processes(known_pids);
		for (auto& proc : proc_sessions) {
			sessions.push_back(std::move(proc));
		}

		if (sessions.size() > 25)
			sessions.erase(sessions.begin(), sessions.end() - 25);

		cached = sessions;
		return cached;
	}

	bool register_click(const string& session_id) {
		const uint64_t now = Tools::time_ms();
		const uint64_t delta = now - last_click_time;
		bool is_double = (!last_click_session.empty() && last_click_session == session_id && delta < 500);
		last_click_time = now;
		last_click_session = session_id;
		focused = true;
		for (size_t i = 0; i < last_sessions.size(); ++i) {
			if (last_sessions[i].session_id == session_id) {
				selected_index = static_cast<int>(i);
				break;
			}
		}
		return is_double;
	}

	static string shell_escape_single(const string& value) {
		if (value.empty())
			return "''";
		string out = "'";
		for (const char c : value) {
			if (c == '\'') {
				out += "'\"'\"'";
			} else {
				out += c;
			}
		}
		out += "'";
		return out;
	}

	void attach_session(const string& session_id) {
		if (!session_id.starts_with("agent-")) {
			return;
		}
		string check_cmd = fmt::format("tmux has-session -t '{}' 2>/dev/null", session_id);
		int exists = std::system(check_cmd.c_str());
		if (exists != 0) {
			return;
		}
		string attach_cmd = fmt::format("tmux attach -t '{}'", session_id);
		std::system(attach_cmd.c_str());
	}

	static SessionInfo* selected_session() {
		if (last_sessions.empty()) return nullptr;
		if (selected_index < 0) selected_index = 0;
		if (selected_index >= static_cast<int>(last_sessions.size())) selected_index = static_cast<int>(last_sessions.size()) - 1;
		return &last_sessions[selected_index];
	}

	void toggle_focus() {
		focused = !focused;
		redraw = true;
	}

	bool handle_nav_key(const std::string_view key) {
		if (!focused || last_sessions.empty() || last_visible_rows <= 0) {
			return false;
		}

		if (key == "up") {
			if (selected_index > 0) selected_index--;
		} else if (key == "down") {
			if (selected_index < static_cast<int>(last_sessions.size()) - 1) selected_index++;
		} else if (key == "page_up") {
			selected_index = max(0, selected_index - last_visible_rows);
		} else if (key == "page_down") {
			selected_index = min(static_cast<int>(last_sessions.size()) - 1, selected_index + last_visible_rows);
		} else if (key == "home") {
			selected_index = 0;
		} else if (key == "end") {
			selected_index = static_cast<int>(last_sessions.size()) - 1;
		} else {
			return false;
		}

		redraw = true;
		return true;
	}

	bool resume_selected() {
		auto* entry = selected_session();
		if (!entry) return false;
		if (!entry->session_id.starts_with("agent-")) return false;
		if (entry->command.empty()) return false;

		string check_cmd = fmt::format("tmux has-session -t '{}' 2>/dev/null", entry->session_id);
		int exists = std::system(check_cmd.c_str());
		if (exists == 0) {
			attach_session(entry->session_id);
			return true;
		}

		string cmd = fmt::format("tmux new-session -d -s '{}' -- bash -lc {}",
			entry->session_id,
			shell_escape_single(entry->command));
		int result = std::system(cmd.c_str());
		if (result == 0) {
			attach_session(entry->session_id);
			return true;
		}
		return false;
	}

	bool activate_selected() {
		auto* entry = selected_session();
		if (!entry) return false;
		if (entry->running) {
			attach_session(entry->session_id);
			return true;
		}
		return resume_selected();
	}

	bool kill_selected() {
		auto* entry = selected_session();
		if (!entry) return false;
		if (!entry->running) return false;
		if (!entry->session_id.starts_with("agent-")) return false;
		string cmd = fmt::format("tmux kill-session -t '{}' 2>/dev/null", entry->session_id);
		std::system(cmd.c_str());
		return true;
	}

	string draw(const vector<SessionInfo>& sessions, bool force_redraw, bool /*no_update*/) {
		string out;
		if (redraw || force_redraw)
			out += box;

		for (auto it = Input::mouse_mappings.begin(); it != Input::mouse_mappings.end();) {
			if (it->first.starts_with("agent:")) {
				it = Input::mouse_mappings.erase(it);
			} else {
				++it;
			}
		}

		const int inner_width = max(0, width - 2);
		const int rows = max(0, height - 2);
		if (inner_width == 0 || rows == 0) {
			redraw = false;
			return out;
		}

		int session_w = min(18, max(10, inner_width / 4));
		int type_w = 6;
		int pid_w = 6;
		int status_w = 7;
		int time_w = 8;
		int cmd_w = inner_width - (session_w + type_w + pid_w + status_w + time_w + 4);

		if (cmd_w < 8) {
			pid_w = 0;
			time_w = 0;
			cmd_w = inner_width - (session_w + type_w + status_w + 2);
		}

		if (cmd_w < 8) {
			type_w = 0;
			cmd_w = inner_width - (session_w + status_w + 1);
		}

		auto build_row = [&](const string& session, const string& type, const string& pid, const string& status, const string& time, const string& cmd) {
			string row;
			row += Tools::ljust(session, session_w, true);
			row += " ";
			if (type_w > 0) {
				row += Tools::ljust(type, type_w, true);
				row += " ";
			}
			if (pid_w > 0) {
				row += Tools::rjust(pid, pid_w, true);
				row += " ";
			}
			row += Tools::ljust(status, status_w, true);
			if (time_w > 0) {
				row += " ";
				row += Tools::ljust(time, time_w, true);
			}
			if (cmd_w > 0) {
				row += " ";
				row += Tools::ljust(cmd, cmd_w, true);
			}
			return Tools::ljust(row, inner_width, true);
		};

		string header = build_row("Session:", "Type:", "Pid:", "Status:", "Time:", "Command:");
		out += Mv::to(y + 1, x + 1) + Theme::c("title") + Fx::b + header + Fx::ub + Theme::c("main_fg");

		const int max_entries = max(0, rows - 1);
		last_visible_rows = max_entries;
		int line = 0;
		auto sorted = sessions;
		std::reverse(sorted.begin(), sorted.end());
		last_sessions = sorted;
		if (selected_index < 0) selected_index = 0;
		if (selected_index >= static_cast<int>(last_sessions.size())) selected_index = static_cast<int>(last_sessions.size()) - 1;
		if (selected_index < scroll_offset) scroll_offset = selected_index;
		if (selected_index >= scroll_offset + max_entries) scroll_offset = max(0, selected_index - max_entries + 1);

		const int end_index = min(static_cast<int>(sorted.size()), scroll_offset + max_entries);
		for (int i = scroll_offset; i < end_index; ++i) {
			const auto& entry = sorted[i];
			if (line >= max_entries)
				break;
			string status = entry.running ? (entry.waiting ? "Waiting" : "Running") : "Stopped";
			string status_color = entry.running ? (entry.waiting ? Theme::c("hi_fg") : Theme::c("proc_misc")) : Theme::c("inactive_fg");
			string row = build_row(entry.session_id, entry.agent_type, (entry.pid ? std::to_string(entry.pid) : "-"), status, entry.time_str, entry.command);
			const bool is_selected = (focused && i == selected_index);
			if (is_selected) {
				out += Mv::to(y + 2 + line, x + 1) + Theme::c("selected_bg") + Theme::c("selected_fg") + row + Theme::c("main_fg");
			} else {
				out += Mv::to(y + 2 + line, x + 1) + Theme::c("main_fg") + row;
			}
			out += Mv::to(y + 2 + line, x + 1 + session_w + 1 + (type_w > 0 ? type_w + 1 : 0) + (pid_w > 0 ? pid_w + 1 : 0))
				+ status_color + status + Theme::c("main_fg");
			Input::mouse_mappings[fmt::format("agent:{}", entry.session_id)] = {y + 2 + line, x + 1, 1, inner_width};
			line++;
		}

		if (sessions.empty() && max_entries > 0) {
			string msg = Tools::ljust("No agent sessions found", inner_width, true);
			out += Mv::to(y + 2, x + 1) + Theme::c("inactive_fg") + msg + Theme::c("main_fg");
			line = 1;
		}

		for (; line < max_entries; ++line) {
			out += Mv::to(y + 2 + line, x + 1) + string(inner_width, ' ');
		}

		redraw = false;
		return out + Fx::reset;
	}
}
