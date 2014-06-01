#include <algorithm>
#include "fcontext.hpp"
#include <iostream>
#include <ecalc/single_handlist.hpp>

namespace Freedom5 {

FContext::FContext(const Value &data, FContextConfig *_config)
    : config(_config), last_action(Action(ActionType::None, bb(0))) {
  pot = data["pot"].GetDouble();
  highest_bet = data["highest_bet"].GetDouble();
  index_bot = data["index_bot"].GetInt();
  index_utg = data["index_utg"].GetInt();
  index_button = data["index_button"].GetInt();
  betting_round = data["betting_round"].GetInt();
  index_active = index_bot;
  phase = load_phase(data);
  load_players(data);

  // set the bot players hand. this is important or he will be having a
  // randomhandlist.
  // TODO delete this pointer on descturct?
  bot_seat().player.handlist = new ecalc::SingleHandlist(config->bot_hand);
}

FContext::FContext(int _index_bot, int _index_utg, int _index_button,
                   int _index_active, vector<FSeat> _seats,
                   FContextConfig *_config)
    : pot(0), highest_bet(0), index_bot(_index_bot), index_utg(_index_utg),
      index_button(_index_button), index_active(_index_active),
      betting_round(0), phase(PhaseType::Preflop), seats(_seats),
      last_action(Action(ActionType::None, bb(0))), config(_config) {}

FContext::FContext(bb _pot, bb _highest_bet, int _index_bot, int _index_utg,
                   int _index_button, int _index_active, int _betting_round,
                   PhaseType::Enum _phase, vector<FSeat> _seats,
                   Action _last_action, FContextConfig *_config)
    : pot(_pot), highest_bet(_highest_bet), index_bot(_index_bot),
      index_utg(_index_utg), index_button(_index_button),
      index_active(_index_active), betting_round(_betting_round), phase(_phase),
      seats(_seats), last_action(_last_action), config(_config) {}

FContext::FContext(const FContext &fc)
    : pot(fc.pot), highest_bet(fc.highest_bet), index_bot(fc.index_bot),
      index_utg(fc.index_utg), index_button(fc.index_button),
      index_active(fc.index_active), betting_round(fc.betting_round),
      phase(fc.phase), seats(fc.seats), last_action(fc.last_action),
      config(fc.config) {}

FContext &FContext::operator=(const FContext &fc) {
  pot = fc.pot;
  highest_bet = fc.highest_bet;
  index_bot = fc.index_bot;
  index_utg = fc.index_utg;
  index_button = fc.index_button;
  index_active = fc.index_active;
  betting_round = fc.betting_round;
  phase = fc.phase;
  seats = fc.seats;
  last_action = fc.last_action;
  config = fc.config;
  return *this;
}

FContext::~FContext() {}

PhaseType::Enum FContext::load_phase(const Value &data) {
  string str_phase = data["phase"].GetString();
  if (str_phase == "preflop")
    return PhaseType::Preflop;
  if (str_phase == "flop")
    return PhaseType::Flop;
  if (str_phase == "turn")
    return PhaseType::Turn;
  if (str_phase == "river")
    return PhaseType::River;
  // TODO throw
}

void FContext::load_players(const Value &data) {
  for (SizeType i = 0; i < data["player"].Size(); ++i)
    seats.push_back(FSeat(data["player"][i]));
}

FContext FContext::transition(Action action) const {
  FContext ncontext = FContext(*this);

  if (ncontext.is_terminal())
    throw std::runtime_error("Terminal context can't transition");

  ncontext.last_action = action;

  bool ok;
  bb bankroll, investment, to_call, raise;
  bankroll = active_seat().player.bankroll;
  investment = active_seat().player.invested[phase];

  switch (action.action) {
  case ActionType::Fold:
    ncontext.active_seat().set_inactive();
    break;
  case ActionType::Check:
    break;
  case ActionType::Call:
    to_call = action.amount;
    ok = ncontext.active_seat().player.make_investment(to_call, phase);

    if (ok) {
      ncontext.pot += to_call;
    } else {
      ncontext.pot += bankroll;
      ncontext.active_seat().player.invested[phase] += bankroll;
      ncontext.active_seat().player.bankroll = bb(0);
    }

    if (bankroll <= to_call)
      ncontext.active_seat().set_allin();

    break;
  case ActionType::Raise:
  case ActionType::AllIn:
    raise = action.amount;
    ok = ncontext.active_seat().player.make_investment(raise, phase);

    if (ok) {
      // transaction successfull
      ncontext.pot += raise;
      ncontext.highest_bet = ncontext.active_seat().player.invested[phase];
    } else {
      // raise > bankroll
      ncontext.pot += bankroll;
      // can only pay partially. add amount to invested manually
      ncontext.active_seat().player.invested[phase] += bankroll;

      ncontext.highest_bet =
          (ncontext.highest_bet <
           ncontext.active_seat().player.invested[phase])
              ? ncontext.active_seat().player.invested[phase]
              : ncontext.highest_bet;

      ncontext.active_seat().player.bankroll = bb(0);
    }

    if (bankroll <= raise)
      ncontext.active_seat().set_allin();

    ncontext.index_utg = index_active;
    ++ncontext.betting_round;
    break;
  }

  // track players action sequence
  ncontext.active_seat().player.action_sequence.append(action,
                                                        ncontext.phase,betting_round);

  if (ncontext.is_last_to_act()) {
    ncontext.transition_phase();
  } else {
    // checks if utg_index is currently reachable and acts if it isnt.
    // to prevent bad transitions
    if (!ncontext.is_utg_index_reachable()) {
      ncontext.index_utg = ncontext.next_to_act();
    }

    ncontext.index_active = ncontext.next_to_act();
  }

  return ncontext;
}

bool FContext::is_utg_index_reachable() const {
  return seats[index_utg].is_active();
}

vector<FContext> FContext::transition() const {
  vector<FContext> new_contexts;
  vector<Action> actions = available_actions(); // TODO we know size

  std::for_each(actions.begin(), actions.end(),
                [&](Action &a) { new_contexts.push_back(transition(a)); });
  return new_contexts;
}

bool FContext::is_last_to_act() const {
  return (next_to_act() == index_utg || next_to_act() == -1);
}

vector<ActionType::Enum> FContext::enum_available_actions() const {
  vector<ActionType::Enum> actions;

  // no more actions if we are done
  if (phase == PhaseType::Showdown)
    return actions;

  bb bankroll = seats[index_active].player.bankroll;
  bb to_call = highest_bet - seats[index_active].player.invested[phase];
  to_call = (bankroll > to_call) ? to_call : bankroll;

  if (to_call > bb(0))
    actions.push_back(ActionType::Fold);
  else
    actions.push_back(ActionType::Check);

  if (to_call > bb(0))
    actions.push_back(ActionType::Call);

  if (betting_round < config->betting_rounds && (to_call < bankroll)) {
    actions.push_back(ActionType::Raise); // make bet distinction here?
  }

  return actions;
}

vector<Action> FContext::available_actions() const {
  vector<Action> actions;
  vector<double> sizes;

  bb bankroll = seats[index_active].player.bankroll;
  bb to_call = highest_bet - seats[index_active].player.invested[phase];
  to_call = (bankroll > to_call) ? to_call : bankroll;

  /* enumerate each action and wrap it in an action object.
   * bet/raise size calculation + split sizes between bot and opponents
   */
  for (auto paction : enum_available_actions()) {
    switch (paction) {
    case ActionType::Fold:
      actions.push_back(Action(paction, bb(0)));
      break;
    case ActionType::Check:
      actions.push_back(Action(paction, bb(0)));
      break;
    case ActionType::Call:
      actions.push_back(Action(paction, to_call));
      break;
    case ActionType::Raise:
      if (index_active == index_bot) {
        sizes = has_bet() ? config->raise_sizes : config->bet_sizes;
        for (unsigned i = 0; i < sizes.size(); ++i) {
          bb raise_amount = get_bet_raise_amount(sizes[i]);
          bb real_raise =
              raise_amount - seats[index_active].player.invested[phase];
          // allin used here
          if (real_raise >= seats[index_active].player.bankroll) {
            actions.push_back(
                Action(ActionType::Raise,
                       seats[index_active].player.bankroll +
                           seats[index_active].player.invested[phase]));
            return actions;
          }
          actions.push_back(Action(ActionType::Raise, raise_amount));
        }
      } else {
        // model für betsize hier TODO
        bb raise_amount = get_bet_raise_amount(has_bet() ? 2 : 0.6);
        bb real_raise =
            raise_amount - seats[index_active].player.invested[phase];
        if (real_raise >= seats[index_active].player.bankroll)
          // allin
          actions.push_back(
              Action(ActionType::Raise,
                     seats[index_active].player.bankroll +
                         seats[index_active].player.invested[phase]));
        else
          actions.push_back(Action(ActionType::Raise, raise_amount));
      }
    }
  }

  return actions;
}

void FContext::transition_phase() {
  // change phase
  if (nb_player_active() < 2)
    phase = PhaseType::Showdown;
  else
    phase = (PhaseType::Enum)(phase + 1);

  // highest bet and bettingr. are 0 again
  highest_bet = 0;
  betting_round = 0;

  // set utg player ( new phase its the player after the button )
  index_utg = next_utg();
  index_active = index_utg;

  /**
   * if utg index is now -1 no player is left to act
   * for the entire game. so we deal all remaining board-
   * cards and move to showdown.
   **/
  if (index_utg == -1 && phase != PhaseType::Showdown) {
    transition_phase();
  }
}

bb FContext::get_bet_raise_amount(double factor) const {
  return has_bet() ? (highest_bet * bb(factor)) : (pot * bb(factor));
}

bool FContext::has_bet() const { return (highest_bet != bb(0)); }

bool FContext::is_terminal() const {
  return (phase == PhaseType::Showdown ||
          (nb_player_active() < 2 && index_active == -1) ||
          (nb_player_active() == 1 &&
           get_last_active_seat().player.invested[phase] == highest_bet) ||
          seats[index_bot].is_inactive());
}

FSeat FContext::get_last_active_seat() const {
  for (int i = 0; i < seats.size(); ++i) {
    if (seats[i].is_active())
      return seats[i];
  }
  // TODO throw
}

int FContext::nb_player_active() const {
  return std::count_if(seats.begin(), seats.end(),
                       [](const FSeat &s) { return s.is_active(); });
}

int FContext::nb_player_allin() const {
  return std::count_if(seats.begin(), seats.end(),
                       [](const FSeat &s) { return s.is_allin(); });
}

int FContext::nb_player_inactive() const {
  return std::count_if(seats.begin(), seats.end(),
                       [](const FSeat &s) { return s.is_inactive(); });
}

int FContext::nb_player_not_inactive() const {
  return std::count_if(seats.begin(), seats.end(),
                       [](const FSeat &s) { return !s.is_inactive(); });
}

FSeat &FContext::bot_seat() { return seats[index_bot]; }
const FSeat &FContext::bot_seat() const { return seats[index_bot]; }

FSeat &FContext::active_seat() { return seats[index_active]; }
const FSeat &FContext::active_seat() const { return seats[index_active]; }

int FContext::next_utg() const {
  int next;
  for (unsigned i = 1; i < seats.size(); ++i) {
    next = (index_button + i) % seats.size();
    if (next != index_button && seats[next].is_active())
      return next;
  }
  return -1;
}

int FContext::next_to_act() const {
  int next;
  for (unsigned i = 1; i < seats.size(); ++i) {
    next = (index_active + i) % seats.size();
    if (seats[next].is_active() &&
        seats[next].player.invested[phase] <= highest_bet &&
        next != index_active)
      return next;
  }
  return -1;
}

void FContext::serialize_fields(Writer<FileStream> &writer) {
  writer.StartArray();

  writer.String("phase");
  writer.String("pot");
  writer.String("highest_bet");
  writer.String("betting_round");
  writer.String("index_bot");
  writer.String("index_active");
  writer.String("index_utg");
  writer.String("index_button");
  writer.String("last_action");
  writer.String("player");

  writer.EndArray();
}

void FContext::serialize(Writer<FileStream> &writer) {
  writer.StartArray();

  writer.String(PhaseType::ToStr[phase]);
  writer.Double(pot.getAsDouble());
  writer.Double(highest_bet.getAsDouble());
  writer.Int(betting_round);
  writer.Int(index_bot);
  writer.Int(index_active);
  writer.Int(index_utg);
  writer.Int(index_button);

  writer.StartArray();
  writer.String(last_action.str().c_str());
  writer.Double(last_action.amount.getAsDouble());
  writer.EndArray();

  writer.StartArray();
  for (auto &s : seats) {
    s.serialize(writer);
  }
  writer.EndArray();

  writer.EndArray();
}
};