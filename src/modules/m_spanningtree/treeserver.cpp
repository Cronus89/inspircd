/*
 * InspIRCd -- Internet Relay Chat Daemon
 *
 *   Copyright (C) 2009 Daniel De Graaf <danieldg@inspircd.org>
 *   Copyright (C) 2007-2008 Craig Edwards <craigedwards@brainbox.cc>
 *   Copyright (C) 2008 Robin Burchell <robin+git@viroteck.net>
 *   Copyright (C) 2007 Dennis Friis <peavey@inspircd.org>
 *
 * This file is part of InspIRCd.  InspIRCd is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "inspircd.h"
#include "xline.h"
#include "main.h"
#include "modules/spanningtree.h"

#include "utils.h"
#include "treeserver.h"

/** We use this constructor only to create the 'root' item, Utils->TreeRoot, which
 * represents our own server. Therefore, it has no route, no parent, and
 * no socket associated with it. Its version string is our own local version.
 */
TreeServer::TreeServer()
	: Parent(NULL), Route(NULL), ServerName(ServerInstance->Config->ServerName), ServerDesc(ServerInstance->Config->ServerDesc)
	, VersionString(ServerInstance->GetVersionString()), Socket(NULL), sid(ServerInstance->Config->GetSID()), ServerUser(ServerInstance->FakeClient)
	, age(ServerInstance->Time()), Warned(false), bursting(false), UserCount(0), OperCount(0), rtt(0), StartBurst(0), Hidden(false)
{
	AddHashEntry();
}

/** When we create a new server, we call this constructor to initialize it.
 * This constructor initializes the server's Route and Parent, and sets up
 * its ping counters so that it will be pinged one minute from now.
 */
TreeServer::TreeServer(const std::string& Name, const std::string& Desc, const std::string& id, TreeServer* Above, TreeSocket* Sock, bool Hide)
	: Parent(Above), ServerName(Name), ServerDesc(Desc), Socket(Sock), sid(id), ServerUser(new FakeUser(id, Name))
	, age(ServerInstance->Time()), Warned(false), bursting(true), UserCount(0), OperCount(0), rtt(0), Hidden(Hide)
{
	SetNextPingTime(ServerInstance->Time() + Utils->PingFreq);
	SetPingFlag();

	long ts = ServerInstance->Time() * 1000 + (ServerInstance->Time_ns() / 1000000);
	this->StartBurst = ts;
	ServerInstance->Logs->Log(MODNAME, LOG_DEBUG, "Server %s started bursting at time %lu", sid.c_str(), ts);

	/* find the 'route' for this server (e.g. the one directly connected
	 * to the local server, which we can use to reach it)
	 *
	 * In the following example, consider we have just added a TreeServer
	 * class for server G on our network, of which we are server A.
	 * To route traffic to G (marked with a *) we must send the data to
	 * B (marked with a +) so this algorithm initializes the 'Route'
	 * value to point at whichever server traffic must be routed through
	 * to get here. If we were to try this algorithm with server B,
	 * the Route pointer would point at its own object ('this').
	 *
	 *            A
	 *           / \
	 *        + B   C
	 *         / \   \
	 *        D   E   F
	 *       /         \
	 *    * G           H
	 *
	 * We only run this algorithm when a server is created, as
	 * the routes remain constant while ever the server exists, and
	 * do not need to be re-calculated.
	 */

	Route = Above;
	if (Route == Utils->TreeRoot)
	{
		Route = this;
	}
	else
	{
		while (this->Route->GetParent() != Utils->TreeRoot)
		{
			this->Route = Route->GetParent();
		}
	}

	/* Because recursive code is slow and takes a lot of resources,
	 * we store two representations of the server tree. The first
	 * is a recursive structure where each server references its
	 * children and its parent, which is used for netbursts and
	 * netsplits to dump the whole dataset to the other server,
	 * and the second is used for very fast lookups when routing
	 * messages and is instead a hash_map, where each item can
	 * be referenced by its server name. The AddHashEntry()
	 * call below automatically inserts each TreeServer class
	 * into the hash_map as it is created. There is a similar
	 * maintainance call in the destructor to tidy up deleted
	 * servers.
	 */

	this->AddHashEntry();
}

const std::string& TreeServer::GetID()
{
	return sid;
}

void TreeServer::FinishBurstInternal()
{
	this->bursting = false;
	SetNextPingTime(ServerInstance->Time() + Utils->PingFreq);
	SetPingFlag();
	for (ChildServers::const_iterator i = Children.begin(); i != Children.end(); ++i)
	{
		TreeServer* child = *i;
		child->FinishBurstInternal();
	}
}

void TreeServer::FinishBurst()
{
	FinishBurstInternal();
	ServerInstance->XLines->ApplyLines();
	long ts = ServerInstance->Time() * 1000 + (ServerInstance->Time_ns() / 1000000);
	unsigned long bursttime = ts - this->StartBurst;
	ServerInstance->SNO->WriteToSnoMask(Parent == Utils->TreeRoot ? 'l' : 'L', "Received end of netburst from \2%s\2 (burst time: %lu %s)",
		ServerName.c_str(), (bursttime > 10000 ? bursttime / 1000 : bursttime), (bursttime > 10000 ? "secs" : "msecs"));
	AddServerEvent(Utils->Creator, ServerName);
}

int TreeServer::QuitUsers(const std::string &reason)
{
	const char* reason_s = reason.c_str();
	std::vector<User*> time_to_die;
	for (user_hash::iterator n = ServerInstance->Users->clientlist->begin(); n != ServerInstance->Users->clientlist->end(); n++)
	{
		if (n->second->server == ServerName)
		{
			time_to_die.push_back(n->second);
		}
	}
	for (std::vector<User*>::iterator n = time_to_die.begin(); n != time_to_die.end(); n++)
	{
		User* a = (User*)*n;
		if (!IS_LOCAL(a))
		{
			if (Utils->quiet_bursts)
				a->quietquit = true;

			if (ServerInstance->Config->HideSplits)
				ServerInstance->Users->QuitUser(a, "*.net *.split", reason_s);
			else
				ServerInstance->Users->QuitUser(a, reason_s);
		}
	}
	return time_to_die.size();
}

/** This method is used to add the structure to the
 * hash_map for linear searches. It is only called
 * by the constructors.
 */
void TreeServer::AddHashEntry()
{
	Utils->serverlist[ServerName] = this;
	Utils->sidlist[sid] = this;
}

/** These accessors etc should be pretty self-
 * explanitory.
 */
TreeServer* TreeServer::GetRoute()
{
	return Route;
}

const std::string& TreeServer::GetDesc()
{
	return ServerDesc;
}

const std::string& TreeServer::GetVersion()
{
	return VersionString;
}

void TreeServer::SetNextPingTime(time_t t)
{
	this->NextPing = t;
	LastPingWasGood = false;
}

time_t TreeServer::NextPingTime()
{
	return NextPing;
}

bool TreeServer::AnsweredLastPing()
{
	return LastPingWasGood;
}

void TreeServer::SetPingFlag()
{
	LastPingWasGood = true;
}

TreeSocket* TreeServer::GetSocket()
{
	return Socket;
}

TreeServer* TreeServer::GetParent()
{
	return Parent;
}

void TreeServer::SetVersion(const std::string &Version)
{
	VersionString = Version;
}

void TreeServer::AddChild(TreeServer* Child)
{
	Children.push_back(Child);
}

bool TreeServer::DelChild(TreeServer* Child)
{
	std::vector<TreeServer*>::iterator it = std::find(Children.begin(), Children.end(), Child);
	if (it != Children.end())
	{
		Children.erase(it);
		return true;
	}
	return false;
}

/** Removes child nodes of this node, and of that node, etc etc.
 * This is used during netsplits to automatically tidy up the
 * server tree. It is slow, we don't use it for much else.
 */
void TreeServer::Tidy()
{
	while (1)
	{
		std::vector<TreeServer*>::iterator a = Children.begin();
		if (a == Children.end())
			return;
		TreeServer* s = *a;
		s->Tidy();
		s->cull();
		Children.erase(a);
		delete s;
	}
}

CullResult TreeServer::cull()
{
	if (!IsRoot())
		ServerUser->cull();
	return classbase::cull();
}

TreeServer::~TreeServer()
{
	/* We'd better tidy up after ourselves, eh? */
	if (!IsRoot())
		delete ServerUser;

	Utils->sidlist.erase(sid);
	Utils->serverlist.erase(ServerName);
}
