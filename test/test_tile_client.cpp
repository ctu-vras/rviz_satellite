/* Copyright 2026

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include <gtest/gtest.h>

#include <QBuffer>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QImage>
#include <QLocale>
#include <QTcpServer>
#include <QTcpSocket>
#include <QCoreApplication>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>

#include "tile_client.hpp"

using rviz_satellite::TileClient;
using rviz_satellite::TileCoordinate;
using rviz_satellite::TileId;
using rviz_satellite::tile_request_error;

namespace
{
/// a PNG image the fake tile server can serve
QByteArray imageBytes()
{
  QImage image(4, 4, QImage::Format_RGB32);
  image.fill(Qt::red);
  QByteArray bytes;
  QBuffer buffer(&bytes);
  buffer.open(QIODevice::WriteOnly);
  image.save(&buffer, "PNG");
  return bytes;
}

/// how the fake tile server answers the requests
enum class Response
{
  Ok,               ///< 200 with an image, cacheable
  ServerError,      ///< 500, which could be just a temporary glitch
  NotFound,         ///< 404, the tile will never appear
  NotAnswering,     ///< the connection is taken, but the tile server never replies
};

/**
 * @brief Tile server on the local host, which always answers the same way.
 */
class FakeTileServer
{
public:
  explicit FakeTileServer(Response response) : response_(response)
  {
    QObject::connect(&server_, &QTcpServer::newConnection, [this] { answer(); });
    if (!server_.listen(QHostAddress::LocalHost, 0)) {
      throw std::runtime_error("Fake tile server cannot listen: " + server_.errorString().toStdString());
    }
  }

  /// URL of this server, unique for every instance so the tile cache is never hit by mistake
  std::string url() const
  {
    return "http://127.0.0.1:" + std::to_string(server_.serverPort()) + "/{z}/{y}/{x}?run=" +
           std::to_string(run_);
  }

  /// how many tiles were so far requested from this server
  int requests() const { return requests_; }

private:
  void answer()
  {
    auto socket = server_.nextPendingConnection();
    ++requests_;
    QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
      socket->readAll();
      if (response_ == Response::NotAnswering) {
        return;
      }
      const QByteArray body = response_ == Response::Ok ? imageBytes() : QByteArray();
      const int status =
        response_ == Response::Ok ? 200 : (response_ == Response::NotFound ? 404 : 500);
      // the date is required for the tile server’s answers to be interpretable as cacheable
      const QString date = QLocale(QLocale::English, QLocale::UnitedStates)
                             .toString(QDateTime::currentDateTimeUtc(), "ddd, dd MMM yyyy HH:mm:ss 'GMT'");
      QByteArray response;
      response += "HTTP/1.1 " + QByteArray::number(status) + " Response\r\n";
      response += "Date: " + date.toUtf8() + "\r\n";
      response += "Content-Type: image/png\r\n";
      response += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
      response += "Cache-Control: max-age=86400, public\r\n\r\n";
      socket->write(response + body);
      socket->flush();
      socket->disconnectFromHost();
    });
  }

  QTcpServer server_;
  Response response_;
  int requests_ = 0;
  const int run_ = static_cast<int>(QCoreApplication::applicationPid() * 100 + counter_++);
  static int counter_;
};

int FakeTileServer::counter_ = 0;

TileId tile(const std::string & url, int x = 3, int y = 5)
{
  return TileId{url, TileCoordinate{x, y, 12}};
}

/**
 * @brief Runs the Qt event loop until the condition holds, at most for the given time.
 * @return true if the condition became true
 */
bool runUntil(const std::function<bool()> & condition, int timeout_ms = 4000)
{
  QElapsedTimer timer;
  timer.start();
  while (!condition()) {
    if (timer.elapsed() > timeout_ms) {
      return false;
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

/// checks the image of the request is not available yet
template<typename Future>
bool isPending(const Future & future)
{
  return future.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}
}  // namespace

/**
 * A tile which cannot be retrieved stays requested and is repeatedly asked for, and the client
 * reports why it does not arrive.
 */
TEST(TileClient, retriesFailingRequestsAndReportsThem)
{
  FakeTileServer server{Response::ServerError};
  TileClient client;

  auto future = client.request(tile(server.url()));
  EXPECT_TRUE(isPending(future));
  EXPECT_TRUE(client.error().empty());

  // the failing tile is requested repeatedly, until it is finally received
  ASSERT_TRUE(runUntil([&server] { return server.requests() >= 3; }));
  EXPECT_TRUE(isPending(future));
  EXPECT_NE(std::string::npos, client.error().find("repeating it")) << client.error();

  // ... and the request is not lost, so once the server recovers, the image is delivered
  client.cancelRequests();
  EXPECT_THROW(future.get(), tile_request_error);
}

/**
 * Regression test: a failed request used to stay in the client forever, so the same tile could
 * never be requested again and the map stayed broken until the display was recreated.
 */
TEST(TileClient, failingTileCanBeRequestedAgain)
{
  FakeTileServer server{Response::NotFound};
  TileClient client;

  auto future = client.request(tile(server.url()));
  ASSERT_TRUE(runUntil([&future] { return !isPending(future); }));
  EXPECT_THROW(future.get(), tile_request_error);
  EXPECT_NE(std::string::npos, client.error().find("failed")) << client.error();
  const int requests = server.requests();
  ASSERT_EQ(requests, 1) << "a tile the server does not have must not be requested repeatedly";

  // the client has to be ready for a new request of the same tile, it may not throw or hang
  auto repeated = client.request(tile(server.url()));
  EXPECT_TRUE(isPending(repeated));
  ASSERT_TRUE(runUntil([&server, requests] { return server.requests() > requests; }));
  ASSERT_TRUE(runUntil([&repeated] { return !isPending(repeated); }));
  EXPECT_THROW(repeated.get(), tile_request_error);
}

/**
 * Regression test: requesting a tile which is already on its way may not fail, and may not
 * duplicate the traffic.
 */
TEST(TileClient, requestsOfPendingTileAreShared)
{
  FakeTileServer server{Response::NotAnswering};
  TileClient client;

  const auto tile_id = tile(server.url());
  auto first = client.request(tile_id);
  ASSERT_TRUE(runUntil([&server] { return server.requests() >= 1; }));
  auto second = client.request(tile_id);

  EXPECT_TRUE(isPending(first));
  EXPECT_TRUE(isPending(second));
  EXPECT_EQ(server.requests(), 1) << "the tile which is already arriving was requested again";

  client.cancelRequests();
  EXPECT_THROW(first.get(), tile_request_error);
  EXPECT_THROW(second.get(), tile_request_error);
}

/**
 * Regression test: the requests which nobody waits for are cancelled, so their images are not
 * downloaded, and the client is ready for new requests afterwards.
 */
TEST(TileClient, cancelledRequestsDoNotHangAndCanBeRepeated)
{
  FakeTileServer server{Response::NotAnswering};
  TileClient client;

  const auto tile_id = tile(server.url());
  auto dropped = client.request(tile_id);
  ASSERT_TRUE(runUntil([&server] { return server.requests() >= 1; }));

  client.cancelRequests();
  EXPECT_THROW(dropped.get(), tile_request_error);

  auto repeated = client.request(tile_id);
  EXPECT_TRUE(isPending(repeated));
  ASSERT_TRUE(runUntil([&server] { return server.requests() >= 2; }));
  client.cancelRequests();
  EXPECT_THROW(repeated.get(), tile_request_error);
}

/**
 * Only the tile which is not displayed anymore is dropped, the other requests stay in place.
 */
TEST(TileClient, onlyRequestedTileIsCancelled)
{
  FakeTileServer server{Response::NotAnswering};
  TileClient client;

  auto cancelled = client.request(tile(server.url(), 1));
  auto kept = client.request(tile(server.url(), 2));
  ASSERT_TRUE(runUntil([&server] { return server.requests() >= 2; }));

  client.cancelRequest(tile(server.url(), 1));
  EXPECT_THROW(cancelled.get(), tile_request_error);
  EXPECT_TRUE(isPending(kept));

  client.cancelRequests();
  EXPECT_THROW(kept.get(), tile_request_error);
}

/**
 * A received tile is stored in the cache, so it is not retrieved from the internet again.
 */
TEST(TileClient, receivedTilesAreServedFromCache)
{
  FakeTileServer server{Response::Ok};
  TileClient client;

  const auto tile_id = tile(server.url());
  auto future = client.request(tile_id);
  ASSERT_TRUE(runUntil([&future] { return !isPending(future); }));
  const QImage image = future.get();
  ASSERT_FALSE(image.isNull());
  EXPECT_TRUE(client.error().empty());
  ASSERT_EQ(server.requests(), 1);

  // the very same tile has to be delivered without another request to the tile server
  const TileId other{server.url(), TileCoordinate{3, 5, 12}};
  auto cached = client.request(other);
  ASSERT_TRUE(runUntil([&cached] { return !isPending(cached); }));
  EXPECT_FALSE(cached.get().isNull());
  EXPECT_EQ(server.requests(), 1) << "the cached tile was downloaded again";
}

int main(int argc, char ** argv)
{
  // the display is a GUI plugin, but only Qt's networking and image classes are used here
  QCoreApplication app(argc, argv);
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
