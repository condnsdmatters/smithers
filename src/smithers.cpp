#include "smithers.h"

#include <zmq.hpp>
#include <m2pp.hpp>
#include <json/json.h>

#include <iostream>
#include <sstream>
#include <random>
#include <algorithm> 


namespace {

std::string gen_random(const size_t len)
{
    char s[len];
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

    std::random_device gen;

    for (size_t i = 0; i < len; ++i) {
        s[i] = alphanum[gen() % (sizeof(alphanum) - 1)];
    }
    s[len] = 0; // null
    
    return std::string(s);
}

void log_request(const m2pp::request& req)
{
    std::ostringstream log_request;

    log_request << "<pre>" << std::endl;
    log_request << "SENDER: " << req.sender << std::endl;
    log_request << "IDENT: " << req.conn_id << std::endl;
    log_request << "PATH: " << req.path << std::endl;
    log_request << "BODY: " << req.body << std::endl;
    
    for (std::vector<m2pp::header>::const_iterator it=req.headers.cbegin();it!=req.headers.cend();it++) {
        log_request << "HEADER: " << it->first << ": " << it->second << std::endl;
    }

    log_request << "</pre>" << std::endl;

    std::cout << log_request.str();
}

bool is_dealer(const smithers::Player& p)
{
    return p.m_is_dealer;
}

void mark_broke_players(std::vector<smithers::Player>& players)
{
    for (size_t i=0; i<players.size(); i++)
    {
        if (players[i].m_chips<=0)
        {
            players[i].m_in_play = false;
        };
    }
}

bool is_active(const smithers::Player& p)
{
    return (p.m_chips>0);
}

} // close anon namespace

namespace smithers{

Smithers::Smithers():
    m_zmq_context(1),
    m_publisher(m_zmq_context, ZMQ_PUB)
{
        m_publisher.bind("tcp://127.0.0.1:9950");
}


void Smithers::await_registered_players(int max_players, int max_chips)
{
    std::cout << "await_registered_players().." << std::endl;

    m2pp::connection conn("UUID_1", "tcp://127.0.0.1:9997", "tcp://127.0.0.1:9996"); 
    
    int seat = 1;
    while (true){
        m2pp::request req = conn.recv();

        if (req.disconnect) {
            std::cout << "== disconnect ==" << std::endl;
            continue;
        }

        log_request(req);

        Json::Value root;
        Json::Reader reader;

        bool was_success = reader.parse( req.body, root );
        if ( !was_success ){
            std::cout  << "Failed to parse configuration\n"
                       << reader.getFormattedErrorMessages();
            return;
        }
 
        std::string default_name =  "Player" + std::to_string(seat);
        std::string name = root.get("name", default_name).asString();
        


        Player new_player(name, gen_random(100), seat, max_chips);
        
        Json::Value resp_json;
        resp_json["name"] = new_player.m_name;
        resp_json["chips"] = new_player.m_chips;
        resp_json["key"] = new_player.m_hash_key;

        std::ostringstream resp;
        resp << resp_json;


        conn.reply_http(req, resp.str());
        m_players.push_back(new_player);
        if (seat < max_players){
            seat++;
        } else {
            break;
        }

    }
    m_players[0].m_is_dealer = true;

}

void Smithers::publish_to_all(const std::string& message)
{
    std::cout << "ALL: " << message << std::endl;
    zmq::message_t zmq_message(message.begin(), message.end());
    m_publisher.send(zmq_message);
}

void Smithers::publish_to_all(const Json::Value& json)
{
    std::ostringstream message;
    message << json;
    publish_to_all(message.str());
}

void Smithers::play_game()
{
    std::cout << "play_game" << std::endl;
    
    Game new_game;
    std::vector<Hand> hands = new_game.deal_hands(m_players.size()); // need to eject players
    Json::Value dealt_hands = create_dealt_hands_message(hands);
    // add blinds, set dealer
    publish_to_all(dealt_hands);

    std::vector<std::string> side_pots;

    play_betting_round(3, 100, 0, side_pots);

    new_game.deal_flop();
    Json::Value flop = create_table_cards_message(new_game.get_table());
    // add pot
    publish_to_all(flop);

    play_betting_round(1, 100, 0, side_pots);

    new_game.deal_river();
    Json::Value river = create_table_cards_message(new_game.get_table());
    // add pot
    publish_to_all(river);

    play_betting_round(1, 100, 0, side_pots);
    
    new_game.deal_turn();
    Json::Value turn = create_table_cards_message(new_game.get_table());
    // add pot
    publish_to_all(turn);

    play_betting_round(1, 100, 0, side_pots);

    int winning_seats = new_game.return_winning_hand();

    reset_and_move_dealer_to_next_player();

}

void Smithers::play_tournament()
{
    std::cout<< std::count_if(m_players.begin(), m_players.end(), is_active);
    while (std::count_if(m_players.begin(), m_players.end(), is_active) >1){
        play_game();
        mark_broke_players(m_players);
    }

}

Json::Value Smithers::create_dealt_hands_message(const std::vector<Hand>& hands)
{

    int dealer_seat = get_dealer();
    int hand_number = 0;
    Json::Value players(Json::arrayValue);
    for (size_t i=0; i<m_players.size(); ++i){
        
        int deal_to_seat = (dealer_seat + 1 + i) % m_players.size(); // start left of dealer

        if (!m_players[deal_to_seat].m_in_play){
            m_players[deal_to_seat].m_seat = -1; // you aren't playing, have no hand in hands vector
            continue;
        }

        m_players[deal_to_seat].m_seat = hand_number;

        Json::Value player;
        player["name"] = m_players[i].m_name;
        player["chips"] = m_players[i].m_chips;
        player["hand"] << hands[hand_number];

        players.append(player);
        hand_number++;
        
    }

    Json::Value root;
    root["type"] = "DEALT_HANDS";
    root["pot"] = 0;
    root["players"] = players;

    return root;
}

Json::Value Smithers::create_table_cards_message(const std::vector<Card>& cards)
{
    Json::Value card_vector(Json::arrayValue);
    for (std::vector<Card>::const_iterator c_it = cards.cbegin();
        c_it != cards.cend();
        c_it++){
        // ugly
        std::ostringstream c;
        c << *c_it;
        card_vector.append(c.str());
    } 

    Json::Value root;
    root["type"] = "DEALT_HANDS";
    root["pot"] = 0;
    root["cards"] = card_vector;

    return root;
}

Json::Value Smithers::create_move_request(const Player& player, int pot, int last_bet)
{
    Json::Value root;
    root["type"] = "MOVE_REQUEST";
    root["pot"] = pot;
    root["name"] = player.m_name;
    root["last_bet"] = last_bet;
    root["chips"] = player.m_chips - player.m_chips_this_round;
    return root;
}

Json::Value Smithers::create_move_message(const Player& player, enum MoveType move, int amount)
{

    std::string move_string;
    switch (move)
    {
        case FOLD:
            move_string="FOLD";
            amount = 0;
            break;
        case RAISE:
            move_string="RAISE_TO";
            break;
        case CALL:
            move_string="CALL";
            break;
        case ALL_IN:
            move_string="ALL_IN";
            break;
    }

    Json::Value root;
    root["type"] = "MOVE";
    root["move"] = move_string;
    root["bet"] = amount;
    root["name"] = player.m_name;
    root["chips"] = player.m_chips - player.m_chips_this_round;

    return root;
    }


Json::Value Smithers::listen_and_pull_from_queue(const std::string& player_name)
{
    m2pp::connection conn("ID", "tcp://127.0.0.1:9900", "tcp://127.0.0.1:9901"); // is it smart to do this here?
    while (true){
        m2pp::request req = conn.recv();
        if (req.disconnect) 
        {
            std::cout << "== disconnect ==" << std::endl;
            continue;
        } 
        else 
        {
            Json::Value root;
            Json::Reader reader;
            bool was_success = reader.parse(req.body, root);
            if (!was_success || root.get("name", "").asString() != player_name)
            {
                continue;
            }

            conn.reply_http(req, "{}");
            return root;
        }
    }

}

enum MoveType Smithers::process_move(const Json::Value& move, Player& player, int& min_raise, int& last_bet)
{
    std::string this_move = move.get("move", "").asString();
    int this_bet = move.get("chips", "0").asInt();

    if (this_bet + player.m_chips_this_round >= player.m_chips)
    {
        player.m_chips_this_round = player.m_chips;
        player.m_chips_this_game = player.m_chips;
        return ALL_IN;
    }

    if (this_move == "RAISE_TO")
    {
        int new_raise = this_bet - last_bet;
        if (new_raise >= min_raise) 
        {    
            min_raise = new_raise;
            player.m_chips_this_round = this_bet;
            player.m_chips_this_game += this_bet;
            
            last_bet = this_bet;
            return RAISE;  // a real RAISE
        } 
        else if (new_raise > 0)
        {
            player.m_chips_this_round = last_bet;
            player.m_chips_this_game += last_bet;

            return CALL; // raise < min raise -> CALL
        }
        else if (new_raise < 0)
        {
            player.m_in_play_this_round = false;
            return FOLD; // this bet < last bet -> FOLD
        }
    }
    else if (this_move == "CALL")
    {
        if (last_bet > player.m_chips) 
        {
            player.m_chips_this_round = player.m_chips;
            player.m_chips_this_game = player.m_chips;
            return ALL_IN;
        }
        player.m_chips_this_round = last_bet;
        player.m_chips_this_game += last_bet;
        return CALL;
    }
    else if (this_move == "FOLD")
    {
        player.m_in_play_this_round = false;
        return FOLD; // this bet < last bet -> FOLD   
    }
    else
    {
        player.m_in_play_this_round = false;
        return FOLD;
    };

    return FOLD; // no compiler warnings
}

int Smithers::get_pot_value_for_game() //bad unidiomatic
{
    int sum = 0; 
    for (size_t i=0; i<m_players.size(); i++)
    {
        sum += m_players[i].m_chips_this_game;
    }
    return sum;
};


void Smithers::play_betting_round(int first_to_bet, int min_raise, int last_bet, std::vector<std::string>& side_pots)
{

    int to_play_index = get_dealer();
    for (int i=0; i<first_to_bet; i++){
        to_play_index = get_next_to_play(to_play_index);
    }

    std::string to_play_name = m_players[to_play_index].m_name;
    std::string last_to_raise_name = to_play_name;

    do {
        Player& this_player = m_players[to_play_index];

        // 1. Ask for a move (giving info)
        publish_to_all(create_move_request(this_player, get_pot_value_for_game(), last_bet));
        
        // 2. Pull relevant move down
        Json::Value move = listen_and_pull_from_queue(this_player.m_name);
        
        // 3. Process move
        enum MoveType result = process_move(move, this_player, min_raise, last_bet);
        if (result == ALL_IN)
        {
            if (this_player.m_chips_this_round > last_bet + min_raise)
            {
                last_bet = this_player.m_chips_this_round;
                last_to_raise_name = this_player.m_name; 
            }
            side_pots.push_back(this_player.m_name);
            this_player.m_all_in_this_round = true;
        }

        if (result == RAISE)
        {
            last_to_raise_name = this_player.m_name; 
        }
 
        // 4. Tell people about it
        publish_to_all(create_move_message(this_player, result, this_player.m_chips_this_round ));
        
        
        // 5. Move to next player
        to_play_index = get_next_to_play(to_play_index);
        to_play_name =  m_players[to_play_index].m_name; 
    } while (last_to_raise_name != to_play_name);


    // Finally add this round's betting to grand pot
    put_betting_round_in_pot();
}

void Smithers::put_betting_round_in_pot()
{ 
    for (size_t i=0; i<m_players.size(); i++)
    {
        m_players[i].m_chips -= m_players[i].m_chips_this_round; 
        m_players[i].m_chips_this_round = 0;
        std::cout<<m_players[i].m_name << " "<< m_players[i].m_chips_this_game << std::endl;
    }
};

void Smithers::print_players()
{
    std::ostringstream message;
    message << '[';
    for (players_cit_t it = m_players.cbegin();
        it !=  m_players.cend();
        it++)
    {

        message << '{'
         << "\"name\":\"" << it->m_name <<"\", "
         << "\"seat\":\"" << it->m_seat <<"\", "
         << "\"chips\":\"" << it->m_chips <<"\" "
         << "},";

    }
    message << "]" << std::endl;
    publish_to_all(message.str());
}

int Smithers::get_dealer()
{
    std::vector<Player>::iterator it = std::find_if(m_players.begin(), m_players.end(), is_dealer);
    return it - m_players.begin(); 
}

int Smithers::get_next_to_play(int seat)
{
    int next = (seat + 1) % m_players.size();
    if (m_players[next].m_in_play && m_players[next].m_in_play_this_round)
    {
        return next;
    }
    else 
    {
        return get_next_to_play(next);
    }
}

void Smithers::reset_and_move_dealer_to_next_player()
{
    int dealer = get_dealer();
    for (size_t i=0; i<m_players.size(); ++i){
        m_players[i].m_in_play_this_round = true;
    }
    int next_dealer = get_next_to_play(dealer);

    std::cout<< "OLD DEALER: " << dealer << " "<< m_players[dealer].m_name<<std::endl;
    std::cout<< "NEW DEALER: " << next_dealer << " " <<m_players[next_dealer].m_name<<std::endl;
    m_players[dealer].m_is_dealer = false;
    m_players[next_dealer].m_is_dealer = true;

};
} // smithers namespace