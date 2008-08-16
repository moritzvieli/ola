/*
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * universe.cpp
 * Defines the operations on the universe class
 * Copyright (C) 2005  Simon Newton
 *
 */

#include <llad/universe.h>
#include <llad/logger.h>
#include "network.h"
#include "UniverseStore.h"
#include "client.h"

#include <arpa/inet.h>
#include <iterator>
#include <string.h>
#include <stdio.h>

map<int ,Universe *> Universe::uni_map;
Network *Universe::c_net;
UniverseStore *Universe::c_uni_store;

/*
 * Create a new universe
 *
 * @param uid  the universe id of this universe
 */
Universe::Universe(int uid) :
  m_uid(uid),
  m_merge_mode(Universe::MERGE_LTP),
  m_length(DMX_LENGTH),
  m_name("") {

  memset(m_data, 0x00, DMX_LENGTH);
}


/*
 * Destroy this universe
 * Removes this object from the universe map
 *
 */
Universe::~Universe() {
  uni_map.erase(m_uid);
}


/*
 * Get the name of this universe
 *
 * @return the name of this universe
 */
string Universe::get_name() const {
  return m_name;
}


/*
 * Set the name of this universe
 *
 * @param name  the name to give this universe
 */
void Universe::set_name(const string &name, bool save) {
  m_name = name;
  if (c_uni_store != NULL && save)
    c_uni_store->store_uni(this);
}


/*
 * Add a port to this universe
 *
 * @param prt  the port to add
 */
int Universe::add_port(Port *prt) {
  Universe *uni;
  vector<Port*>::iterator it;

  uni = prt->get_universe();

  if (uni == this)
    return 0;

  // unpatch if required
  if (uni != NULL) {
    Logger::instance()->log(Logger::DEBUG, "Port %p is bound to universe %d", prt, uni->get_uid());
    uni->remove_port(prt);

    // destroy this universe if it's no longer in use
    if (!uni->in_use()) {
      delete uni;
    }
  }

  // patch to this universe
  Logger::instance()->log(Logger::INFO, "Patched %d to universe %d",prt->get_id() , m_uid);
  ports_vect.push_back(prt);

  prt->set_universe(this);

  return 0;
}


/*
 * Remove a port from this universe. After calling this method you need to
 * check if this universe is still in use, and if not delete it
 *
 * @param prt the port to remove
 */
int Universe::remove_port(Port *prt) {
  vector<Port*>::iterator it;

  it = find(ports_vect.begin(),ports_vect.end(),prt); // first position

  if (it != ports_vect.end()) {
    ports_vect.erase(it);
    prt->set_universe(NULL);
    Logger::instance()->log(Logger::DEBUG, "Port %p has been removed from uni %d", prt, get_uid());

  } else {
    Logger::instance()->log(Logger::DEBUG, "Could not find port in universe");
    return -1;
  }

  return 0;
}


/*
 * Returns the number of ports assigned to this universe
 *
 * @return  the number of ports in this universe
 */
int Universe::get_num_ports() const {
  return ports_vect.size();
}


/*
 * Add this client to this universe
 *
 * @param cli  the client to add
 *
 */
int Universe::add_client(Client *cli) {
  vector<Client*>::iterator it;

  // add to this universe
  Logger::instance()->log(Logger::INFO, "Added client %p to universe %d", cli, m_uid);
  clients_vect.push_back(cli);

  return 0;

}


/*
 * Remove this client from the universe. After calling this method you need to
 * check if this universe is still in use, and if not delete it
 *
 * @param cli  the client to remove
 */
int Universe::remove_client(Client *cli) {
  vector<Client*>::iterator it;
  it = find(clients_vect.begin(), clients_vect.end(),cli); // first position

  if (it != clients_vect.end()) {
    clients_vect.erase(it);
    Logger::instance()->log(Logger::INFO, "Client %p has been removed from uni %d", cli, get_uid());
  } else {
    Logger::instance()->log(Logger::DEBUG, "Could not find client in universe");
    return -1;
  }
  return 0;
}


/*
 * Set the dmx data for this universe
 *
 * @param  dmx  pointer to the dmx data
 * @param  len  the length of the dmx buffer
 */
int Universe::set_dmx(uint8_t *dmx, int len) {

  if (m_merge_mode == Universe::MERGE_LTP) {
    m_length = len < DMX_LENGTH ? len : DMX_LENGTH;
    memcpy(m_data, dmx, m_length);
  } else {
    // HTP, this is more difficult
    // we'll need a buffer per client
    // for now just set it
    m_length = len < DMX_LENGTH ? len : DMX_LENGTH;
    memcpy(m_data, dmx, m_length);
  }
  return this->update_dependants();
}


/*
 * Set the dmx data for this universe
 *
 * @param dmx    the buffer to copy data into
 * @param length  the length of the buffer
 */
int Universe::get_dmx(uint8_t *dmx, int length) {
  int len;

  len = length < DMX_LENGTH ? length : DMX_LENGTH;
  memcpy(dmx, m_data, len);

  return len;
}

/*
 * Get the universe id for this universe
 *
 * @return the uid
 */
int Universe::get_uid() const {
  return m_uid;
}


/*
 * Call this when the dmx in a port that is part of this universe changes
 *
 * @param prt   the port that has changed
 */
int Universe::port_data_changed(Port *prt) {
  unsigned int i, len;
  int first = 1;

  if ( m_merge_mode == Universe::MERGE_LTP) {
    // LTP merge mode
    // this is simple, find the port and copy the data
    for(i =0; i < ports_vect.size(); i++) {
      if(ports_vect[i] == prt && prt->can_read()) {
        // read the new data and update our dependants
        m_length = prt->read(m_data, DMX_LENGTH);
        update_dependants();
        break;
      }
    }
  } else {
    // htp merge mode
    // iterate over ports which we can read and take the highest value
    // of each channel
    for(i =0; i < ports_vect.size(); i++) {
      if(prt->can_read()) {
        if(first) {
          m_length = prt->read(m_data, DMX_LENGTH);
          first = 0;
        } else {
          len = prt->read(m_merge, DMX_LENGTH);
          merge();
        }
      }
    }
    update_dependants();
  }

  return 0;
}


/*
 * Return true if this universe is in use
 *
 */
bool Universe::in_use() const {
  return ports_vect.size() > 0 || clients_vect.size() > 0;
}


/*
 * Set the merge mode
 */
void Universe::set_merge_mode(Universe::merge_mode mode, bool save) {
  m_merge_mode = mode;
  if (c_uni_store != NULL && save)
    c_uni_store->store_uni(this);

}

/*
 * Get the merge mode
 */
Universe::merge_mode Universe::get_merge_mode() {
  return m_merge_mode;
}


// Private Methods
//-----------------------------------------------------------------------------


/*
 * Called when the dmx data for this universe changes,
 * updates everyone who needs to know (patched ports and network clients)
 *
 */
int Universe::update_dependants() {
  unsigned int i;

  // write to all ports assigned to this unviverse
  for (i=0; i < ports_vect.size(); i++) {
    ports_vect[i]->write(m_data, m_length);
  }

  // write to all clients
  for (i=0; i < clients_vect.size(); i++) {
    send_dmx(clients_vect[i]);
  }

  return 0;
}



/*
 * Send a dmx message to the client
 *
 * @param cli  the client to send the msg to
 *
 */
int Universe::send_dmx(Client *cli) {
  lla_msg reply;

  memset(&reply, 0x00, sizeof(reply));

  reply.to.sin_family = AF_INET;
  reply.to.sin_port = cli->get_port();
  inet_aton(LLAD_ADDR, &reply.to.sin_addr);

  reply.len = sizeof(lla_msg_dmx_data);
  reply.data.dmx.op = LLA_MSG_DMX_DATA;

  memcpy(reply.data.dmx.data, m_data, m_length);
  reply.data.dmx.len = m_length;
  reply.data.dmx.uni = get_uid();

  Logger::instance()->log(Logger::DEBUG, "Sending dmx data msg to client %d", reply.to.sin_port);
  return c_net->send_msg(&reply);
}


/*
 * HTP merge the merge buffer into the data buffer
 *
 */
void Universe::merge() {
  int i, l;

  // l is the length we merge over
  l = m_mlength < m_length ? m_mlength : m_length;

  for (i=0; i < l; i++) {
    m_data[i] = m_data[i] > m_merge[i] ? m_data[i] : m_merge[i];
  }

  if (m_mlength > m_length) {
    // copy the remaining over
    memcpy(&m_data[l], &m_merge[l], m_mlength - m_length);
    m_length = m_mlength;
  }
}


// Class Methods
//-----------------------------------------------------------------------------


/*
 * Lookup a universe from it's address, creates one if it does not exist
 *
 * @param uid  the uid of the required universe
 */
Universe *Universe::get_universe(int uid) {
  map<int , Universe *>::const_iterator iter;

  iter = uni_map.find(uid);
  if (iter != uni_map.end()) {
     return iter->second;
  }

  return NULL;
}

/*
 * Lookup a universe, or create it if it does not exist
 *
 * @param uid  the universe id
 * @return  the universe, or NULL on error
 */
Universe *Universe::get_universe_or_create(int uid) {
  Universe *uni = get_universe(uid);

  if (uni == NULL) {
    uni = new Universe(uid);

    if (uni != NULL) {
      pair<int , Universe*> p (uid, uni);
      uni_map.insert(p);

      if (c_uni_store != NULL)
        c_uni_store->retrieve_uni(uni);
    }
  }

  // this could still be NULL
  return uni;
}


/*
 * delete all universes
 *
 */
int Universe::clean_up() {
  map<int, Universe*>::iterator iter;

  //unload all plugins
  for (iter = uni_map.begin(); iter != uni_map.end();) {
    // increment the iter here
    // the universe will remove itself from the map
    // and invalidate the iter
    delete (*iter++).second;
  }

  uni_map.clear();
  return 0;
}


/*
 * Returns the number of universes active
 *
 * @return   the number of active universes
 */
inline int Universe::universe_count() {
  return uni_map.size();
}


/*
 * Returns a list of universes. This must be freed when you're
 * done with it.
 *
 * @return  the number of entries in the list
 */
vector<Universe *> *Universe::get_list() {
  int numb = Universe::universe_count();
  vector<Universe *> *list = new vector<Universe *>;
  list->reserve(numb);

  map<int ,Universe*>::const_iterator iter;

  // populate list
  for (iter = uni_map.begin(); iter != uni_map.end(); ++iter) {
    list->push_back( iter->second );
  }
  return list;
}


void Universe::check_for_unused() {
  map<int ,Universe*>::const_iterator iter;
  vector<Universe *>::iterator iterv;
  vector<Universe *> list;

  // populate list
  for (iter = uni_map.begin(); iter != uni_map.end(); ++iter) {
    if (! iter->second->in_use()) {
      list.push_back(iter->second);
    }
  }

  for (iterv = list.begin(); iterv != list.end(); ++iterv) {
    delete *iterv;
  }
}


/*
 * Set the network connection this class can use to send msgs
 */
int Universe::set_net(Network *net) {
  c_net = net;
  return 0;
}


/*
 * Set the universe store
 */
int Universe::set_store(UniverseStore *store) {
  c_uni_store = store;
  return 0;
}
