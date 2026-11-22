/* Copyright 2018-2019 TomTom N.V.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */
#pragma once

#include <QImage>
#include <QtNetwork>
#include <future>
#include <map>
#include <memory>
#include <string>

#include "tile.hpp"

namespace rviz_satellite
{

class tile_request_error : public std::exception
{
private:
  std::string message_;

public:
  explicit tile_request_error(const std::string & message) : message_(message) {}

  const char * what() const noexcept override { return message_.c_str(); }
};

/**
 * @brief Download tiles from a Tile server.
 */
class TileClient : public QObject
{
  Q_OBJECT

private:
  /// an image which was requested but not received yet
  struct TileRequest
  {
    std::shared_ptr<std::promise<QImage>> promise;
    std::shared_future<QImage> future;
    QNetworkReply * reply = nullptr;
    /// how often the retrieval of this tile already failed
    int failures = 0;
  };

  /// retrieves the image of the tile now, unless its request was dropped in the meantime
  void sendRequest(const TileId & tile_id);
  /// stops the download of the tile and resolves its future by a tile_request_error
  void cancel(TileRequest && request);

  QNetworkAccessManager * manager_;
  std::map<TileId, TileRequest> tile_requests_;
  /// why the last request failed, empty if the requests currently work
  std::string error_;

  /// delay of the first repeated request, doubled with every next failure of the same tile
  static constexpr int initial_retry_delay_ms = 250;
  /// failing tiles are requested repeatedly with at most this delay, until they finally arrive
  static constexpr int max_retry_delay_ms = 30'000;
  /// how many bytes of tiles the cache on the disk may occupy at most
  static constexpr qint64 max_cache_size = 200LL * 1024 * 1024;

public:
  TileClient();

  /**
   * @brief Load a specific tile
   *
   * Since QNetworkDiskCache is used, tiles will be loaded from the file system if they have been cached. Otherwise they
   * get downloaded.
   *
   * If server url contains "file://", local filesystem will be used.
   *
   * Requests of the same tile are shared, so the tile is not retrieved twice when it is requested
   * again while it is still on its way. A request which fails is repeated with a growing delay
   * until it succeeds, so that a short glitch of the network or of the tile server does not lose
   * the tile. The returned future stays pending as long as the tile is not available, and error()
   * tells why it is not.
   *
   * @return the tile image, or a tile_request_error if the request was cancelled meanwhile
   */
  std::shared_future<QImage> request(const TileId & tile_id);

  /**
   * @brief Load a specific tile from filesystem
   *
   */
  std::shared_future<QImage> request_local(const TileId & tile_id);

  /**
   * @brief Load a specific tile from internet
   *
   * Since QNetworkDiskCache is used, tiles will be loaded from the file system if they have been cached. Otherwise they
   * get downloaded.
   */
  std::shared_future<QImage> request_remote(const TileId & tile_id);

  /**
   * @brief Reason of the last failure of a request, empty if the requests currently work.
   */
  const std::string & error() const
  {
    return error_;
  }

  /**
   * @brief Drop the request of the tile, because nobody waits for its image anymore.
   *
   * Its future is resolved by a tile_request_error and the running download is stopped, so that a
   * tile which is not displayed anymore is not retrieved from the internet either.
   */
  void cancelRequest(const TileId & tile_id);

  /**
   * @brief Drop all pending requests, see cancelRequest().
   */
  void cancelRequests();

private Q_SLOTS:
  void request_finished(QNetworkReply * reply);
};

}  // namespace rviz_satellite
