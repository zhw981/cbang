/******************************************************************************\

          This file is part of the C! library.  A.K.A the cbang library.

                Copyright (c) 2003-2019, Cauldron Development LLC
                   Copyright (c) 2003-2017, Stanford University
                               All rights reserved.

         The C! library is free software: you can redistribute it and/or
        modify it under the terms of the GNU Lesser General Public License
       as published by the Free Software Foundation, either version 2.1 of
               the License, or (at your option) any later version.

        The C! library is distributed in the hope that it will be useful,
          but WITHOUT ANY WARRANTY; without even the implied warranty of
        MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
                 Lesser General Public License for more details.

         You should have received a copy of the GNU Lesser General Public
                 License along with the C! library.  If not, see
                         <http://www.gnu.org/licenses/>.

        In addition, BSD licensing may be granted on a case by case basis
        by written permission from at least one of the copyright holders.
           You may request written permission by emailing the authors.

                  For information regarding this software email:
                                 Joseph Coffland
                          joseph@cauldrondevelopment.com

\******************************************************************************/

#include "HTTP.h"
#include "Base.h"
#include "Request.h"
#include "Event.h"
#include "Connection.h"

#include <cbang/config.h>
#include <cbang/Exception.h>
#include <cbang/Catch.h>
#include <cbang/log/Logger.h>
#include <cbang/time/Timer.h>
#include <cbang/socket/Socket.h>
#include <cbang/openssl/SSLContext.h>
#include <cbang/util/RateSet.h>

using namespace std;
using namespace cb::Event;


HTTP::HTTP(cb::Event::Base &base, const cb::SmartPointer<HTTPHandler> &handler,
           const cb::SmartPointer<cb::SSLContext> &sslCtx) :
  base(base), handler(handler), sslCtx(sslCtx) {

#ifndef HAVE_OPENSSL
  if (!sslCtx.isNull()) THROW("C! was not built with openssl support");
#endif
}


HTTP::~HTTP() {}


void HTTP::setMaxConnectionTTL(unsigned x) {
  maxConnectionTTL = x;

  if (maxConnectionTTL) {
    if (expireEvent.isNull())
      expireEvent = base.newEvent(this, &HTTP::expireCB);

    expireEvent->setPriority(0 < priority ? priority - 1 : priority);
    expireEvent->add(60); // Check once per minute

  } else if (expireEvent->isPending()) expireEvent->del();
}


void HTTP::setEventPriority(int priority) {
  this->priority = priority;

  if (0 <= priority) {
    int p = 0 < priority ? priority - 1 : priority;
    if (expireEvent.isSet()) expireEvent->setPriority(p);
    if (acceptEvent.isSet()) acceptEvent->setPriority(p);
  }
}


void HTTP::remove(Connection &con) {
  connections.remove(&con);
  acceptEvent->add();
}


void HTTP::bind(const cb::IPAddress &addr) {
  // TODO Support binding multiple listener sockets
  if (this->socket.isSet()) THROW("Already bound");

  SmartPointer<Socket> socket = new Socket;
  socket->setReuseAddr(true);
  socket->bind(addr);
  socket->listen(connectionBacklog);
  socket_t fd = socket->get();

  // This event will be destroyed with the HTTP
  acceptEvent = base.newEvent(fd, this, &HTTP::acceptCB,
                              EVENT_READ | EVENT_PERSIST | EVENT_NO_SELF_REF);
  if (0 <= priority)
    acceptEvent->setPriority(0 < priority ? priority - 1 : priority);
  acceptEvent->add();

  this->socket = socket;
  boundAddr = addr;
}


cb::SmartPointer<Request> HTTP::createRequest
(Connection &con, RequestMethod method, const cb::URI &uri,
 const cb::Version &version) {
  return handler->createRequest(con, method, uri, version);
}


void HTTP::handleRequest(Request &req) {
  LOG_DEBUG(5, "New request on " << boundAddr << ", connection count = "
            << getConnectionCount());

  TRY_CATCH_ERROR(dispatch(*handler, req));
}


bool HTTP::dispatch(HTTPHandler &handler, Request &req) {
  try {
    if (handler.handleRequest(req)) {
      handler.endRequest(req);
      return true;
    }

    req.sendError(HTTPStatus::HTTP_NOT_FOUND);

  } catch (cb::Exception &e) {
    if (400 <= e.getCode() && e.getCode() < 600) {
      LOG_WARNING("REQ" << req.getID() << ':' << req.getClientIP() << ':'
                  << e.getMessages());
      req.reply((HTTPStatus::enum_t)e.getCode());

    } else {
      if (!CBANG_LOG_DEBUG_ENABLED(3)) LOG_WARNING(e.getMessages());
      LOG_DEBUG(3, e);
      req.sendError(e);
    }

  } catch (std::exception &e) {
    LOG_ERROR(e.what());
    req.sendError(e);

  } catch (...) {
    LOG_ERROR(HTTPStatus(HTTPStatus::HTTP_INTERNAL_SERVER_ERROR)
              .getDescription());
    req.sendError(HTTPStatus::HTTP_INTERNAL_SERVER_ERROR);
  }

  handler.endRequest(req);
  return false;
}


void HTTP::expireCB() {
  double now = Timer::now();
  unsigned count = 0;

  for (auto it = connections.begin(); it != connections.end();)
    if (maxConnectionTTL < now - (*it)->getStartTime()) {
      it = connections.erase(it);
      if (stats.isSet()) stats->event("timedout");
      count++;

    } else it++;

  LOG_DEBUG(4, "Dropped " << count << " expired connections");
}


void HTTP::acceptCB() {
  if (maxConnections && maxConnections <= connections.size()) {
    handler->evict(connections);
    if (maxConnections <= connections.size()) return acceptEvent->del();
  }

  IPAddress peer;
  auto newSocket = socket->accept(&peer);

  if (newSocket.isNull()) {
    LOG_ERROR("Failed to accept new socket");
    return;
  }

  LOG_DEBUG(4, "New connection from " << peer);

  // Maximize socket buffers
  newSocket->setReceiveBuf();
  newSocket->setSendBuf();

  // Create new Connection
  SmartPointer<Connection> con =
    new Connection(base, true, peer, newSocket, sslCtx);

  con->setHTTP(this);
  con->setMaxHeaderSize(maxHeaderSize);
  con->setMaxBodySize(maxBodySize);
  if (0 <= priority) con->setPriority(priority);
  con->setReadTimeout(readTimeout);
  con->setWriteTimeout(writeTimeout);
  con->setStats(stats);

  connections.push_back(con);
  con->acceptRequest();
}
