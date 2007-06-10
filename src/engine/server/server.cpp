#include <stdio.h>
#include <string.h>

#include <baselib/system.h>

#include <engine/interface.h>

//#include "socket.h"
#include <engine/packet.h>
#include <engine/snapshot.h>

#include <engine/lzw.h>
#include <engine/versions.h>

namespace baselib {}
using namespace baselib;

int net_addr4_cmp(const NETADDR4 *a, const NETADDR4 *b)
{
	if(
		a->ip[0] != b->ip[0] ||
		a->ip[1] != b->ip[1] ||
		a->ip[2] != b->ip[2] ||
		a->ip[3] != b->ip[3] ||
		a->port != b->port
	)
		return 1;
	return 0;
}

// --- string handling (MOVE THESE!!) ---
void snap_encode_string(const char *src, int *dst, int length, int max_length)
{
        const unsigned char *p = (const unsigned char *)src;

        // handle whole int
        for(int i = 0; i < length/4; i++)
        {
                *dst = (p[0]<<24|p[1]<<16|p[2]<<8|p[3]);
                p += 4;
                dst++;
        }

        // take care of the left overs
        int left = length%4;
        if(left)
        {
                unsigned last = 0;
                switch(left)
                {
                        case 3: last |= p[2]<<8;
                        case 2: last |= p[1]<<16;
                        case 1: last |= p[0]<<24;
                }
                *dst = last;
        }
}


class snapshot_builder
{
public:
	static const int MAX_ITEMS = 512;
	//static const int MAX_DATA_SIZE=1*1024;

	char data[MAX_SNAPSHOT_SIZE];
	int data_size;

	int offsets[MAX_ITEMS];
	int num_items;

	int top_size;
	int top_items;

	int snapnum;

	snapshot_builder()
	{
		top_size = 0;
		top_items = 0;
		snapnum = 0;
	}

	void start()
	{
		data_size = 0;
		num_items = 0;
	}

	int finish(void *snapdata)
	{
		snapnum++;

		// collect some data
		/*
		int change = 0;
		if(data_size > top_size)
		{
			change++;
			top_size = data_size;
		}

		if(num_items > top_items)
		{
			change++;
			top_items = num_items;
		}

		if(change)
		{
			dbg_msg("snapshot", "new top, items=%d size=%d", top_items, top_size);
		}*/

		// flattern and make the snapshot
		snapshot *snap = (snapshot *)snapdata;
		snap->num_items = num_items;
		int offset_size = sizeof(int)*num_items;
		mem_copy(snap->offsets, offsets, offset_size);
		mem_copy(snap->data_start(), data, data_size);
		return sizeof(int) + offset_size + data_size;
	}

	void *new_item(int type, int id, int size)
	{
		snapshot::item *obj = (snapshot::item *)(data+data_size);
		obj->type_and_id = (type<<16)|id;
		offsets[num_items] = data_size;
		data_size += sizeof(int) + size;
		num_items++;
		dbg_assert(data_size < MAX_SNAPSHOT_SIZE, "too much data");
		dbg_assert(num_items < MAX_ITEMS, "too many items");

		return &obj->data;
	}
};

static snapshot_builder builder;

void *snap_new_item(int type, int id, int size)
{
	dbg_assert(type >= 0 && type <=0xffff, "incorrect type");
	dbg_assert(id >= 0 && id <=0xffff, "incorrect id");
	return builder.new_item(type, id, size);
}


//
class client
{
public:
	enum
	{
		STATE_EMPTY = 0,
		STATE_CONNECTING = 1,
		STATE_INGAME = 2,
	};

	// connection state info
	int state;

	// (ticks) if lastactivity > 5 seconds kick him
	int64 lastactivity;
	connection conn;

	char name[MAX_NAME_LENGTH];
	char clan[MAX_CLANNAME_LENGTH];
	/*
	client()
	{
		state = STATE_EMPTY;
		name[0] = 0;
		clan[0] = 0;
	}

	~client()
	{
		dbg_assert(state == STATE_EMPTY, "client destoyed while in use");
	}*/

	bool is_empty() const { return state == STATE_EMPTY; }
	bool is_ingame() const { return state == STATE_INGAME; }
	const netaddr4 &address() const { return conn.address(); }
};

static client clients[MAX_CLIENTS];
static int current_tick = 0;
static int send_heartbeats = 1;

int server_tick()
{
	return current_tick;
}

int server_tickspeed()
{
	return 50;
}

int server_init()
{
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		clients[i].state = client::STATE_EMPTY;
		clients[i].name[0] = 0;
		clients[i].clan[0] = 0;
		clients[i].lastactivity = 0;
	}

	current_tick = 0;

	return 0;
}

int server_getclientinfo(int client_id, client_info *info)
{
	dbg_assert(client_id >= 0 && client_id < MAX_CLIENTS, "client_id is not valid");
	dbg_assert(info != 0, "info can not be null");

	if(clients[client_id].is_ingame())
	{
		info->name = clients[client_id].name;
		info->latency = 0;
		return 1;
	}
	return 0;
}

//
class server
{
public:

	socket_udp4 game_socket;

	const char *map_name;
	const char *server_name;
	int64 lasttick;
	int64 lastheartbeat;
	netaddr4 master_server;

	int biggest_snapshot;

	bool run(const char *servername, const char *mapname)
	{
		biggest_snapshot = 0;

		net_init(); // For Windows compatibility.
		map_name = mapname;
		server_name = servername;

		// load map
		if(!map_load(mapname))
		{
			dbg_msg("server", "failed to load map. mapname='%s'");
			return false;
		}

		// start server
		if(!game_socket.open(8303))
		{
			dbg_msg("network/server", "couldn't open socket");
			return false;
		}

		for(int i = 0; i < MAX_CLIENTS; i++)
			dbg_msg("network/server", "\t%d: %d", i, clients[i].state);

		if (net_host_lookup(MASTER_SERVER_ADDRESS, MASTER_SERVER_PORT, &master_server) != 0)
		{
			// TODO: fix me
			//master_server = netaddr4(0, 0, 0, 0, 0);
		}

		mods_init();

		int64 time_per_tick = time_freq()/SERVER_TICK_SPEED;
		int64 time_per_heartbeat = time_freq() * 30;
		int64 starttime = time_get();
		//int64 lasttick = starttime;
		lasttick = starttime;
		lastheartbeat = 0;

		int64 reporttime = time_get();
		int64 reportinterval = time_freq()*3;

		int64 simulationtime = 0;
		int64 snaptime = 0;
		int64 networktime = 0;

		while(1)
		{
			int64 t = time_get();
			if(t-lasttick > time_per_tick)
			{
				{
					int64 start = time_get();
					tick();
					simulationtime += time_get()-start;
				}

				{
					int64 start = time_get();
					snap();
					snaptime += time_get()-start;
				}

				// Check for client timeouts
				for (int i = 0; i < MAX_CLIENTS; i++)
				{
					if (clients[i].state != client::STATE_EMPTY)
					{
						// check last activity time
						if (((lasttick - clients[i].lastactivity) / time_freq()) > SERVER_CLIENT_TIMEOUT)
							client_timeout(i);
					}
				}

				lasttick += time_per_tick;
			}

			if(send_heartbeats)
			{
				if (t > lastheartbeat+time_per_heartbeat)
				{
					if (master_server.port != 0)
					{
						int players = 0;

						for (int i = 0; i < MAX_CLIENTS; i++)
							if (!clients[i].is_empty())
								players++;

						// TODO: fix me
						netaddr4 me(127, 0, 0, 0, 8303);

						send_heartbeat(0, &me, players, MAX_CLIENTS, server_name, mapname);
					}

					lastheartbeat = t+time_per_heartbeat;
				}
			}

			{
				int64 start = time_get();
				pump_network();
				networktime += time_get()-start;
			}

			if(reporttime < time_get())
			{
				int64 totaltime = simulationtime+snaptime+networktime;
				dbg_msg("server/report", "sim=%.02fms snap=%.02fms net=%.02fms total=%.02fms load=%.02f%%",
					simulationtime/(float)reportinterval*1000,
					snaptime/(float)reportinterval*1000,
					networktime/(float)reportinterval*1000,
					totaltime/(float)reportinterval*1000,
					(simulationtime+snaptime+networktime)/(float)reportinterval*100.0f);

				unsigned sent_total=0, recv_total=0;
				for (int i = 0; i < MAX_CLIENTS; i++)
					if (!clients[i].is_empty())
					{
						unsigned s,r;
						clients[i].conn.counter_get(&s,&r);
						clients[i].conn.counter_reset();
						sent_total += s;
						recv_total += r;
					}


				dbg_msg("server/report", "biggestsnap=%d send=%d recv=%d",
					biggest_snapshot, sent_total/3, recv_total/3);

				simulationtime = 0;
				snaptime = 0;
				networktime = 0;

				reporttime += reportinterval;
			}

			thread_sleep(1);
		}

		mods_shutdown();
		map_unload();
	}

	void tick()
	{
		current_tick++;
		mods_tick();
	}

	void snap()
	{
		mods_presnap();

		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(clients[i].is_ingame())
			{
				char data[MAX_SNAPSHOT_SIZE];
				char compdata[MAX_SNAPSHOT_SIZE];
				builder.start();
				mods_snap(i);

				// finish snapshot
				int snapshot_size = builder.finish(data);

				// compress it
				int compsize = lzw_compress(data, snapshot_size, compdata);
				snapshot_size = compsize;

				if(snapshot_size > biggest_snapshot)
					biggest_snapshot = snapshot_size;

				const int max_size = MAX_SNAPSHOT_PACKSIZE;
				int numpackets = (snapshot_size+max_size-1)/max_size;
				for(int n = 0, left = snapshot_size; left; n++)
				{
					int chunk = left < max_size ? left : max_size;
					left -= chunk;

					packet p(NETMSG_SERVER_SNAP);
					p.write_int(numpackets);
					p.write_int(n);
					p.write_int(chunk);
					p.write_raw(&compdata[n*max_size], chunk);
					clients[i].conn.send(&p);
				}
			}
		}

		mods_postsnap();
	}

	void send_accept(client *client, const char *map)
	{
		packet p(NETMSG_SERVER_ACCEPT);
		p.write_str(map);
		client->conn.send(&p);
	}

	void drop(int cid, const char *reason)
	{
		if(clients[cid].state == client::STATE_EMPTY)
			return;

		clients[cid].state = client::STATE_EMPTY;
		mods_client_drop(cid);
		dbg_msg("game", "player dropped. reason='%s' cid=%x name='%s'", reason, cid, clients[cid].name);
	}

	int find_client(const netaddr4 *addr)
	{
		// fetch client
		for(int i = 0; i < MAX_CLIENTS; i++)
		{
			if(!clients[i].is_empty() && clients[i].address() == *addr)
				return i;
		}
		return -1;
	}

	void client_process_packet(int cid, packet *p)
	{
		clients[cid].lastactivity = lasttick;
		if(p->msg() == NETMSG_CLIENT_DONE)
		{
			dbg_msg("game", "player as entered the game. cid=%x", cid);
			clients[cid].state = client::STATE_INGAME;
			mods_client_enter(cid);
		}
		else if(p->msg() == NETMSG_CLIENT_INPUT)
		{
			int input[MAX_INPUT_SIZE];
			int size = p->read_int();
			for(int i = 0; i < size/4; i++)
				input[i] = p->read_int();
			if(p->is_good())
			{
				//dbg_msg("network/server", "applying input %d %d %d", input[0], input[1], input[2]);
				mods_client_input(cid, input);
			}
		}
		else if(p->msg() == NETMSG_CLIENT_ERROR)
		{
			const char *reason = p->read_str();
			if(p->is_good())
				dbg_msg("network/server", "client error. cid=%x reason='%s'", cid, reason);
			else
				dbg_msg("network/server", "client error. cid=%x", cid);
			drop(cid, "client error");
		}
		else
		{
			dbg_msg("network/server", "invalid message. cid=%x msg=%x", cid, p->msg());
			drop(cid, "invalid message");
		}
	}

	void process_packet(packet *p, netaddr4 *from)
	{
		// do version check
		if(p->version() != TEEWARS_NETVERSION)
		{
			// send an empty packet back.
			// this will allow the client to check the version
			packet p;
			game_socket.send(from, p.data(), p.size());
			return;
		}

		if(p->msg() == NETMSG_CLIENT_CONNECT)
		{
			// we got no state for this client yet
			const char *version;
			const char *name;
			const char *clan;
			const char *password;
			const char *skin;

			version = p->read_str();
			name = p->read_str();
			clan = p->read_str();
			password = p->read_str();
			skin = p->read_str();

			if(p->is_good())
			{
				/*
				// check version
				if(strcmp(version, TEEWARS_NETVERSION) != 0)
				{
					dbg_msg("network/server", "wrong version connecting '%s'", version);
					// TODO: send error
					return;
				}*/

				// look for empty slot, linear search
				int id = -1;
				for(int i = 0; i < MAX_CLIENTS; i++)
					if(clients[i].is_empty())
					{
						id = i;
						break;
					}

				if(id != -1)
				{
					// slot found
					// TODO: perform correct copy here
					mem_copy(clients[id].name, name, MAX_NAME_LENGTH);
					mem_copy(clients[id].clan, clan, MAX_CLANNAME_LENGTH);
					clients[id].state = client::STATE_CONNECTING;
					clients[id].conn.init(&game_socket, from);

					clients[id].lastactivity = lasttick;
					clients[id].name[MAX_NAME_LENGTH-1] = 0;
					clients[id].clan[MAX_CLANNAME_LENGTH-1] = 0;

					dbg_msg("network/server", "client connected. '%s' on slot %d", name, id);

					// TODO: return success
					send_accept(&clients[id], map_name);
				}
				else
				{
					// no slot found
					// TODO: send error
					dbg_msg("network/server", "client connected but server is full");

					for(int i = 0; i < MAX_CLIENTS; i++)
						dbg_msg("network/server", "\t%d: %d", i, clients[i].state);
				}
			}
		}
		else
		{
			int cid = find_client(from);
			if(cid >= 0)
			{
				if(clients[cid].conn.feed(p))
				{
					// packet is ok
					unsigned msg = p->msg();

					// client found, check state
					if(((msg>>16)&0xff)&clients[cid].state)
					{
						// state is ok
						client_process_packet(cid, p);
					}
					else
					{
						// invalid state, disconnect the client
						drop(cid, "invalid message at this state");
					}
				}
				else
				{
					drop(cid, "connection error");
				}

			}
			else
				dbg_msg("network/server", "packet from strange address.");
		}
	}

	void client_timeout(int clientId)
	{
		drop(clientId, "client timedout");
	}

	void pump_network()
	{
		while(1)
		{
			packet p;
			netaddr4 from;

			//int bytes = net_udp4_recv(
			int bytes = game_socket.recv(&from, p.data(), p.max_size());
			//int bytes = game_socket.recv(&from, p.data(), p.max_size());
			if(bytes <= 0)
				break;

			process_packet(&p, &from);
		}
		// TODO: check for client timeouts
	}

	char *write_int(char *buffer, int integer)
	{
		*buffer++ = integer >> 24;
		*buffer++ = integer >> 16;
		*buffer++ = integer >> 8;
		*buffer++ = integer;

		return buffer;
	}

	char *write_netaddr4(char *buffer, NETADDR4 *address)
	{
		*buffer++ = address->ip[0];
		*buffer++ = address->ip[1];
		*buffer++ = address->ip[2];
		*buffer++ = address->ip[3];

		return write_int(buffer, address->port);
	}

	void send_heartbeat(int version, netaddr4 *address, int players, int max_players, const char *name, const char *map_name)
	{
		char buffer[216] = {0};
		char *d = buffer;

		d = write_int(d, 'TWHB');
		d = write_int(d, version);
		d = write_netaddr4(d, address);
		d = write_int(d,players);
		d = write_int(d, max_players);

		int len = strlen(name);
		if (len > 128)
			len = 128;

		memcpy(d, name, len);
		d += 128;

		len = strlen(map_name);
		if (len > 64)
			len = 64;

		memcpy(d, map_name, len);
		d += 64;
		game_socket.send(&master_server, buffer, sizeof(buffer));
	}
};

int main(int argc, char **argv)
{
	dbg_msg("server", "starting...");

	const char *mapname = "data/demo.map";
	const char *servername = 0;
	// parse arguments
	for(int i = 1; i < argc; i++)
	{
		if(argv[i][0] == '-' && argv[i][1] == 'm' && argv[i][2] == 0 && argc - i > 1)
		{
			// -m map
			i++;
			mapname = argv[i];
		}
		else if(argv[i][0] == '-' && argv[i][1] == 'n' && argv[i][2] == 0 && argc - i > 1)
		{
			// -n server name
			i++;
			servername = argv[i];
		}
		else if(argv[i][0] == '-' && argv[i][1] == 'p' && argv[i][2] == 0)
		{
			// -p (private server)
			send_heartbeats = 0;
		}
	}

	if(!mapname)
	{
		dbg_msg("server", "no map given (-m MAPNAME)");
		return 0;
	}

	if(!servername)
	{
		dbg_msg("server", "no server name given (-n \"server name\")");
		return 0;
	}

	server_init();
	server s;
	s.run(servername, mapname);
	return 0;
}