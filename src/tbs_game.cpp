/*
	Copyright (C) 2003-2013 by David White <davewx7@gmail.com>
	
    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <algorithm>
#include <string>
#include <boost/algorithm/string.hpp>

#include <stdio.h>

#include "asserts.hpp"
#include "foreach.hpp"
#include "formatter.hpp"
#include "formula.hpp"
#include "formula_object.hpp"
#include "json_parser.hpp"
#include "module.hpp"
#include "preferences.hpp"
#include "random.hpp"
#include "tbs_ai_player.hpp"
#include "tbs_internal_server.hpp"
#include "tbs_game.hpp"
#include "tbs_web_server.hpp"
#include "string_utils.hpp"
#include "unit_test.hpp"
#include "variant_utils.hpp"
#include "wml_formula_callable.hpp"

namespace game_logic {
void flush_all_backed_maps();
}

namespace tbs {

extern http_client* g_game_server_http_client_to_matchmaking_server;

struct game_type {
	game_type() {
	}

	explicit game_type(const variant& value)
	{
		variant functions_var = value["functions"];
		if(functions_var.is_string()) {
			functions.reset(new game_logic::function_symbol_table);
			game_logic::formula f(functions_var, functions.get());
		}

		variant handlers_var = value["handlers"];
		if(handlers_var.is_map()) {
			foreach(const variant& key, handlers_var.get_keys().as_list()) {
				handlers[key.as_string()] = game_logic::formula::create_optional_formula(handlers_var[key], functions.get());
			}
		}
	}

	std::string name;
	boost::shared_ptr<game_logic::function_symbol_table> functions;
	std::map<std::string, game_logic::const_formula_ptr> handlers;
};

std::map<std::string, game_type> generate_game_types() {
	std::cerr << "GENERATE GAME TYPES\n";

	std::map<std::string, game_type> result;

	std::vector<std::string> files;
	module::get_files_in_dir("data/tbs", &files);
	foreach(const std::string& fname, files) {
		if(fname.size() > 4 && std::string(fname.end()-4,fname.end()) == ".cfg") {
			std::string type(fname.begin(), fname.end()-4);
			boost::algorithm::to_lower(type);
			result[type] = game_type(json::parse_from_file("data/tbs/" + fname));
			result[type].name = type;
			std::cerr << "LOADED TBS GAME TYPE: " << type << "\n";
		}
	}
	std::cerr << "DONE GENERATE GAME TYPES\n";

	return result;
}

std::map<std::string, game_type>& all_types() {
	static std::map<std::string, game_type> types = generate_game_types();
	return types;
}

extern std::string global_debug_str;

void game::reload_game_types()
{
	all_types() = generate_game_types();
}

namespace {
game* current_game = NULL;

int generate_game_id() {
	static int id = int(time(NULL));
	return id++;
}

using namespace game_logic;

std::string string_tolower(const std::string& s) 
{
	std::string res(s);
	boost::algorithm::to_lower(res);
	return res;
}
}

game::error::error(const std::string& m) : msg(m)
{
	std::cerr << "game error: " << m << "\n";
}

game_context::game_context(game* g) : old_game_(current_game)
{
	current_game = g;
	g->set_as_current_game(true);
}

game_context::~game_context()
{
	current_game->set_as_current_game(false);
	current_game = old_game_;
}

void game_context::set(game* g)
{
	current_game = g;
	g->set_as_current_game(true);
}

game* game::current()
{
	return current_game;
}

boost::intrusive_ptr<game> game::create(const variant& v)
{
	const variant type_var = v["game_type"];
	if(!type_var.is_string()) {
		return NULL;
	}

	std::string type = type_var.as_string();
	boost::algorithm::to_lower(type);
	std::map<std::string, game_type>::const_iterator type_itor = all_types().find(type);
	if(type_itor == all_types().end()) {
		return NULL;
	}

	boost::intrusive_ptr<game> result(new game(type_itor->second));
	game_logic::map_formula_callable_ptr vars(new game_logic::map_formula_callable);
	vars->add("msg", v);
	result->handle_event("create", vars.get());
	return result;
}

game::game(const game_type& type)
  : type_(type), game_id_(generate_game_id()),
    started_(false), state_(STATE_SETUP), state_id_(0), cycle_(0), tick_rate_(50),
	backup_callable_(NULL)
{
}

game::game(const variant& value)
  : type_(all_types()[string_tolower(value["type"].as_string())]),
	game_id_(generate_game_id()),
    started_(value["started"].as_bool(false)),
	state_(STATE_SETUP),
	state_id_(0),
	cycle_(value["cycle"].as_int(0)),
	tick_rate_(value["tick_rate"].as_int(50)),
	backup_callable_(NULL)
{
}

game::game(const std::string& game_type, const variant& doc)
  : type_(all_types()[game_type]),
    game_id_(doc["id"].as_int()),
	started_(doc["started"].as_bool()),
	state_(started_ ? STATE_PLAYING : STATE_SETUP),
	state_id_(doc["state_id"].as_int()),
	cycle_(doc["cycle"].as_int(0)),
	tick_rate_(doc["tick_rate"].as_int(50)),
	backup_callable_(NULL),
	doc_(doc["state"])
{
	log_ = util::split(doc["log"].as_string(), '\n');

	variant players_val = doc["players"];
	for(int n = 0; n != players_val.num_elements(); ++n) {
		add_player(players_val[n].as_string());
	}
}

game::~game()
{
	std::cerr << "DESTROY GAME\n";
}

void game::cancel_game()
{
	players_.clear();
	outgoing_messages_.clear();
	doc_ = variant();
	ai_.clear();
	bots_.clear();
	backup_callable_ = NULL;
	std::cerr << "CANCEL GAME: " << refcount() << "\n";
}

variant game::write(int nplayer, int processing_ms) const
{
	game_logic::wml_formula_callable_serialization_scope serialization_scope;

	variant_builder result;
	result.add("id", game_id_);
	result.add("type", "game");
	result.add("game_type", variant(type_.name));
	result.add("started", variant::from_bool(started_));
	result.add("state_id", state_id_);

	if(processing_ms != -1) {
		fprintf(stderr, "ZZZ: server_time: %d\n", processing_ms);
		result.add("server_time", processing_ms);
	}

	//observers see the perspective of the first player for now
	result.add("nplayer", variant(nplayer < 0 ? 0 : nplayer));

	std::vector<variant> players_val;
	foreach(const player& p, players_) {
		players_val.push_back(variant(p.name));
	}

	result.add("players", variant(&players_val));

	if(current_message_.empty() == false) {
		result.add("message", current_message_);
	}

	if(nplayer < 0) {
		result.add("observer", true);
	}

	bool send_delta = false;

	if(nplayer >= 0 & nplayer < players_.size() && players_[nplayer].state_id_sent != -1 && players_[nplayer].state_id_sent == players_[nplayer].confirmed_state_id && players_[nplayer].allow_deltas) {
		send_delta = true;
	}

	variant state_doc;

	if(type_.handlers.count("transform")) {
		variant msg = formula_object::deep_clone(doc_);
		game_logic::map_formula_callable_ptr vars(new game_logic::map_formula_callable);
		vars->add("message", msg);
		vars->add("nplayer", variant(nplayer < 0 ? 0 : nplayer));
		const_cast<game*>(this)->handle_event("transform", vars.get());

		state_doc = msg;
	} else {
		state_doc = formula_object::deep_clone(doc_);
	}

	if(send_delta) {
		result.add("delta", formula_object::generate_diff(players_[nplayer].state_sent, state_doc));
		result.add("delta_basis", players_[nplayer].confirmed_state_id);
	} else {
		result.add("state", state_doc);
	}

	if(nplayer >= 0 && nplayer < players_.size() && players_[nplayer].allow_deltas) {
		players_[nplayer].state_id_sent = state_id_;
		players_[nplayer].state_sent = state_doc;
	}

	std::string log_str;
	foreach(const std::string& s, log_) {
		if(!log_str.empty()) {
			log_str += "\n";
		}
		log_str += s;
	}

	result.add("log", variant(log_str));

	variant res = result.build();
	res.add_attr(variant("serialized_objects"), serialization_scope.write_objects(res));
	return res;
}

void game::start_game()
{
	if(started_) {
		send_notify("The game has started.");
	}

	state_ = STATE_PLAYING;
	started_ = true;
	handle_event("start");

	send_game_state();

	ai_play();
}

void game::swap_outgoing_messages(std::vector<message>& msg)
{
	outgoing_messages_.swap(msg);
	outgoing_messages_.clear();
}

void game::queue_message(const std::string& msg, int nplayer)
{
	outgoing_messages_.push_back(message());
	outgoing_messages_.back().contents = msg;
	if(nplayer >= 0) {
		outgoing_messages_.back().recipients.push_back(nplayer);
	}

	fprintf(stderr, "QUEUE MESSAGE %d (((%s)))\n", nplayer, msg.c_str());
}

void game::queue_message(const char* msg, int nplayer)
{
	queue_message(std::string(msg), nplayer);
}

void game::queue_message(const variant& msg, int nplayer)
{
	queue_message(msg.write_json(), nplayer);
}

void game::send_error(const std::string& msg, int nplayer)
{
	variant_builder result;
	result.add("type", "error");
	result.add("message", msg);
	queue_message(result.build(), nplayer);
}

void game::send_notify(const std::string& msg, int nplayer)
{
	variant_builder result;
	result.add("type", "message");
	result.add("message", msg);
	queue_message(result.build(), nplayer);
}

game::player::player() : confirmed_state_id(-1), state_id_sent(-1), allow_deltas(true)
{
}

void game::add_player(const std::string& name)
{
	players_.push_back(player());
	players_.back().name = name;
	players_.back().side = players_.size() - 1;
	players_.back().is_human = true;
}

void game::add_ai_player(const std::string& name, const variant& info)
{
	players_.push_back(player());
	players_.back().name = name;
	players_.back().side = players_.size() - 1;
	players_.back().is_human = false;

	handle_event("add_bot", map_into_callable(info).get());

//	boost::intrusive_ptr<bot> new_bot(new bot(*web_server::service(), "localhost", formatter() << web_server::port(), info));
//	bots_.push_back(new_bot);
}

void game::remove_player(const std::string& name)
{
	for(int n = 0; n != players_.size(); ++n) {
		if(players_[n].name == name) {
			players_.erase(players_.begin() + n);
			for(int m = 0; m != ai_.size(); ++m) {
				if(ai_[m]->player_id() == n) {
					ai_.erase(ai_.begin() + m);
					break;
				}
			}

			break;
		}
	}
}

std::vector<std::string> game::get_ai_players() const
{
	std::vector<std::string> result;
	foreach(boost::shared_ptr<ai_player> a, ai_) {
		ASSERT_LOG(a->player_id() >= 0 && a->player_id() < players_.size(), "BAD AI INDEX: " << a->player_id());
		result.push_back(players_[a->player_id()].name);
	}

	return result;
}

int game::get_player_index(const std::string& nick) const
{
	int nplayer = 0;
	foreach(const player& p, players_) {
		if(p.name == nick) {
			foreach(boost::shared_ptr<ai_player> ai, ai_) {
				if(ai->player_id() == nplayer) {
					return -1;
				}
			}
			return nplayer;
		}

		++nplayer;
	}

	return -1;
}

void game::send_game_state(int nplayer, int processing_ms)
{
	fprintf(stderr, "SEND GAME STATE: %d\n", nplayer);
	if(nplayer == -1) {
		for(int n = 0; n != players().size(); ++n) {
			send_game_state(n, processing_ms);
		}

		//Send to observers.
		queue_message(write(-1));
		outgoing_messages_.back().recipients.push_back(-1);

		current_message_ = "";
	} else if(nplayer >= 0 && nplayer < players().size()) {
		queue_message(write(nplayer, processing_ms), nplayer);
	}
}

void game::ai_play()
{
	for(int n = 0; n != ai_.size(); ++n) {
		for(;;) {
			variant msg = ai_[n]->play();
			if(msg.is_null()) {
				break;
			}

			handle_message(ai_[n]->player_id(), msg);
		}
	}
}

void game::set_message(const std::string& msg)
{
	current_message_ = msg;
}

void game::process()
{
	if(db_client_) {
		db_client_->process(100);
	}

	if(started_) {
		const int starting_state_id = state_id_;
		static const std::string ProcessStr = "process";
		handle_event(ProcessStr);
		++cycle_;

		if(state_id_ != starting_state_id) {
			send_game_state();
		}
	}
}

variant game::get_value(const std::string& key) const
{
	if(key == "game") {
		return variant(this);
	} else if(key == "doc") {
		return doc_;
	} else if(key == "state_id") {
		return variant(state_id_);
	} else if(key == "cycle") {
		return variant(cycle_);
	} else if(key == "bots") {
		std::vector<variant> v;
		for(int n = 0; n != bots_.size(); ++n) {
			v.push_back(variant(bots_[n].get()));
		}

		return variant(&v);
	} else if(key == "db_client") {
#ifdef USE_DB_CLIENT
		if(db_client_.get() == NULL) {
			db_client_ = db_client::create();
		}

		return variant(db_client_.get());
#else
		return variant();
#endif
	} else if(key == "players_disconnected") {
		std::vector<variant> result;
		for(auto n : players_disconnected_) {
			result.push_back(variant(n));
		}

		return variant(&result);
	} else if(backup_callable_) {
		return backup_callable_->query_value(key);
	} else {
		return variant();
	}
}

PREF_BOOL(tbs_game_exit_on_winner, false, "If true, tbs games will immediately exit when there is a winner.");

void game::set_value(const std::string& key, const variant& value)
{
	if(key == "doc") {
		doc_ = value;
	} else if(key == "event") {
		if(value.is_string()) {
			handle_event(value.as_string());
		} else if(value.is_map()) {
			handle_event(value["event"].as_string(), map_into_callable(value["arg"]).get());
		}
	} else if(key == "log_message") {
		if(!value.is_null()) {
			log_.push_back(value.as_string());
		}
	} else if(key == "bots") {
		bots_.clear();
		for(int n = 0; n != value.num_elements(); ++n) {
			std::cerr << "BOT_ADD: " << value[n].write_json() << "\n";
			if(value[n].is_callable() && value[n].try_convert<tbs::bot>()) {
				bots_.push_back(boost::intrusive_ptr<tbs::bot>(value[n].try_convert<tbs::bot>()));
			} else if(preferences::internal_tbs_server()) {
				boost::intrusive_ptr<bot> new_bot(new bot(tbs::internal_server::get_io_service(), "localhost", "23456", value[n]));
				bots_.push_back(new_bot);
			} else {
				boost::intrusive_ptr<bot> new_bot(new bot(*web_server::service(), "localhost", formatter() << web_server::port(), value[n]));
				bots_.push_back(new_bot);
			}
		}
	} else if(key == "state_id") {
		state_id_ = value.as_int();
		fprintf(stderr, "XXX: @%d state_id = %d\n", SDL_GetTicks(), state_id_);
	} else if(key == "winner") {
		std::cout << "WINNER: " << value.write_json() << std::endl;

		if(g_game_server_http_client_to_matchmaking_server != NULL) {
			http_client& client = *g_game_server_http_client_to_matchmaking_server;
			variant_builder msg;
			msg.add("type", "server_finished_game");
			msg.add("pid", static_cast<int>(getpid()));

			bool complete = false;

			client.send_request("POST /server", msg.build().write_json(),
			  [&complete](std::string response) {
				complete = true;
			  },
			  [&complete](std::string msg) {
				complete = true;
				ASSERT_LOG(false, "Could not connect to server: " << msg);
			  },
			  [](int a, int b, bool c) {
			  });
			
			while(!complete) {
				client.process();
			}
		}

		if(g_tbs_game_exit_on_winner) {
			game_logic::flush_all_backed_maps();
			_exit(0);
		}
	} else if(backup_callable_) {
		backup_callable_->mutate_value(key, value);
	}
}

void game::handle_message(int nplayer, const variant& msg)
{
	fprintf(stderr, "HANDLE MESSAGE (((%s)))\n", msg.write_json().c_str());
	const std::string type = msg["type"].as_string();
	if(type == "start_game") {
		start_game();
		return;
	} else if(type == "request_updates") {
		if(msg.has_key("state_id") && !doc_.is_null()) {
			if(nplayer >= 0 && nplayer < players_.size() && msg.has_key("allow_deltas") && msg["allow_deltas"].as_bool() == false) {
				players_[nplayer].allow_deltas = false;
			}

			const variant state_id = msg["state_id"];
			if(state_id.as_int() != state_id_ && nplayer >= 0) {
				players_[nplayer].confirmed_state_id = state_id.as_int();
				send_game_state(nplayer);
			} else if(state_id.as_int() == state_id_ && nplayer >= 0 && nplayer < players_.size() && players_[nplayer].confirmed_state_id != state_id_) {

				fprintf(stderr, "XXX: @%d player %d confirm sync %d\n", SDL_GetTicks(), nplayer, state_id_);
				players_[nplayer].confirmed_state_id = state_id_;

				std::ostringstream s;
				s << "{ type: \"confirm_sync\", player: " << nplayer << ", state_id: " << state_id_ << " }";
				std::string msg = s.str();
				for(int n = 0; n != players_.size(); ++n) {
					if(n != nplayer && players_[n].is_human) {
						queue_message(msg, nplayer);
					}
				}
			}
		}
		return;
	} else if(type == "chat_message") {
		variant m = msg;
		if(nplayer >= 0) {
			m.add_attr_mutation(variant("nick"), variant(players_[nplayer].name));
		} else {
			std::string nick = "observer";
			if(m.has_key(variant("nick"))) {
				nick = m["nick"].as_string();
				nick += " (obs)";
			}

			m.add_attr_mutation(variant("nick"), variant(nick));
		}
		queue_message(m);
		return;
	} else if(type == "ping_game") {
		variant_builder response;
		response.add("type", "pong_game");
		response.add("payload", msg);
		queue_message(response.build().write_json(), nplayer);
		return;
	}

	int start_time = SDL_GetTicks();

	game_logic::map_formula_callable_ptr vars(new game_logic::map_formula_callable);
	vars->add("message", msg);
	vars->add("player", variant(nplayer));
	handle_event("message", vars.get());

	const int time_taken = SDL_GetTicks() - start_time;

	fprintf(stderr, "XXX: @%d HANDLED MESSAGE %s\n", SDL_GetTicks(), type.c_str());

	send_game_state(-1, time_taken);
}

void game::setup_game()
{
}

namespace {
struct backup_callable_scope {
	game_logic::formula_callable** ptr_;
	game_logic::formula_callable* backup_;
	backup_callable_scope(game_logic::formula_callable** ptr, game_logic::formula_callable* var) : ptr_(ptr), backup_(*ptr) {
		if(var) {
			*ptr_ = var;
		} else {
			ptr_ = NULL;
		}
	}

	~backup_callable_scope() {
		if(ptr_) {
			*ptr_ = backup_;
		}
	}
};
}

void game::handle_event(const std::string& name, game_logic::formula_callable* variables)
{
	const backup_callable_scope backup_scope(&backup_callable_, variables);

	std::map<std::string, game_logic::const_formula_ptr>::const_iterator itor = type_.handlers.find(name);
	if(itor == type_.handlers.end() || !itor->second) {
		return;
	}

	variant v = itor->second->execute(*this);
	execute_command(v);
}

void game::execute_command(variant cmd)
{
	if(cmd.is_list()) {
		for(int n = 0; n != cmd.num_elements(); ++n) {
			execute_command(cmd[n]);
		}
	} else if(cmd.is_callable()) {
		const game_logic::command_callable* command = cmd.try_convert<game_logic::command_callable>();
		if(command) {
			command->run_command(*this);
		}
	} else if(cmd.is_map()) {
		if(cmd.has_key("execute")) {
			game_logic::formula f(cmd["execute"]);
			game_logic::formula_callable_ptr callable(map_into_callable(cmd["arg"]));
			ASSERT_LOG(callable.get(), "NO ARG SPECIFIED IN EXECUTE AT " << cmd.debug_location());
			variant v = f.execute(*callable);
			execute_command(v);
		}
	}
}

void game::player_disconnect(int nplayer)
{
	variant_builder result;
	result.add("type", "player_disconnect");
	result.add("player", players()[nplayer].name);
	variant msg = result.build();
	for(int n = 0; n != players().size(); ++n) {
		if(n != nplayer) {
			queue_message(msg, n);
		}
	}
	
}

void game::player_reconnect(int nplayer)
{
	variant_builder result;
	result.add("type", "player_reconnect");
	result.add("player", players()[nplayer].name);
	variant msg = result.build();
	for(int n = 0; n != players().size(); ++n) {
		if(n != nplayer) {
			queue_message(msg, n);
		}
	}
}

void game::player_disconnected_for(int nplayer, int time_ms)
{
	if(time_ms >= 60000 && std::find(players_disconnected_.begin(), players_disconnected_.end(), nplayer) == players_disconnected_.end()) {
		players_disconnected_.push_back(nplayer);
		handle_event("player_disconnected");
		send_game_state();
	}
}

}

namespace {
bool g_create_bot_game = false;
void create_game_return(const std::string& msg) {
	g_create_bot_game = true;
	std::cerr << "GAME CREATED\n";
}

void start_game_return(const std::string& msg) {
	std::cerr << "GAME STARTED\n";
}
}

COMMAND_LINE_UTILITY(tbs_bot_game) {
	using namespace tbs;
	using namespace game_logic;

	bool found_create_game = false;
	variant create_game_request = json::parse("{type: 'create_game', game_type: 'citadel', users: [{user: 'a', bot: true, bot_type: 'goblins', session_id: 1}, {user: 'b', bot: true, bot_type: 'goblins', session_id: 2}]}");
	for(int i = 0; i != args.size(); ++i) {
		if(args[i] == "--request" && i+1 != args.size()) {
			create_game_request = json::parse(args[i+1]);
			found_create_game = true;
		}
	}

	ASSERT_LOG(found_create_game, "MUST PROVIDE --request");

	variant start_game_request = json::parse("{type: 'start_game'}");

	boost::intrusive_ptr<map_formula_callable> callable(new map_formula_callable);
	boost::intrusive_ptr<internal_client> client(new internal_client);
	client->send_request(create_game_request, -1, callable, create_game_return);
	while(!g_create_bot_game) {
		internal_server::process();
	}

	client->send_request(start_game_request, 1, callable, start_game_return);

	for(;;) {
		internal_server::process();
	}
}
