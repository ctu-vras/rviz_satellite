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

#include "tile_client.hpp"

#include <QImage>
#include <QImageReader>
#include <QStandardPaths>
#include <QString>
#include <QTimer>
#include <QtCore>
#include <QtNetwork>
#include <algorithm>
#include <chrono>
#include <numeric>
#include <regex>
#include <sstream>
#include <utility>

#include "rviz_common/logging.hpp"

namespace rviz_satellite
{
namespace
{
QImage flipVertically(QImage image)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
  return std::move(image).flipped(Qt::Vertical);
#else
  return std::move(image).mirrored(false, true);
#endif
}

/// checks whether asking the tile server for the tile once again could possibly succeed
bool isTemporaryError(QNetworkReply::NetworkError error)
{
  // if the server claims it does not have the tile or does not allow accessing it, repeating the
  // request will not change its mind, but anything else could have been just a network glitch
  return error != QNetworkReply::ContentNotFoundError &&
         error != QNetworkReply::ContentAccessDenied &&
         error != QNetworkReply::ContentOperationNotPermittedError &&
         error != QNetworkReply::AuthenticationRequiredError;
}

std::string toString(const TileId & tile_id)
{
  std::stringstream stream;
  stream << tile_id;
  return stream.str();
}
}  // namespace

TileClient::TileClient() : manager_(new QNetworkAccessManager(this)), tile_requests_()
{
  connect(manager_, SIGNAL(finished(QNetworkReply *)), SLOT(request_finished(QNetworkReply *)));
  QNetworkDiskCache * disk_cache = new QNetworkDiskCache(this);
  QString const cache_path =
    QDir(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation))
      .filePath("rviz_satellite");
  disk_cache->setCacheDirectory(cache_path);
  // Qt keeps only the last 50 MB of responses, which is exhausted by a few screens of the zoomed-in
  // map, and the display would keep downloading the tiles it actually has cached already
  disk_cache->setMaximumCacheSize(max_cache_size);
  RVIZ_COMMON_LOG_INFO_STREAM(
    "Caching map tiles in " << cache_path.toStdString() << ", at most " <<
      max_cache_size / (1024 * 1024) << " MB");
  manager_->setCache(disk_cache);
}

/**
 * @brief Request a specific tile
 *
 * Since QNetworkDiskCache is used, tiles will be loaded from the file system if they have been cached.
 * Otherwise they are fetched from the tile server given in the @p tile_id.
 */
std::shared_future<QImage> TileClient::request(TileId const & tile_id)
{
  if (tile_id.server_url.find("file://") != std::string::npos) {
    return request_local(tile_id);
  }

  auto pending = tile_requests_.find(tile_id);
  if (pending != tile_requests_.end()) {
    // this tile is already on its way, so it is not worth retrieving it from the server twice
    return pending->second.future;
  }
  return request_remote(tile_id);
}

std::shared_future<QImage> TileClient::request_remote(TileId const & tile_id)
{
  auto entry = tile_requests_.emplace(tile_id, TileRequest{});
  auto & tile_request = entry.first->second;
  if (entry.second) {
    tile_request.promise = std::make_shared<std::promise<QImage>>();
    tile_request.future = tile_request.promise->get_future();
    sendRequest(tile_id);
  }
  return tile_request.future;
}

void TileClient::sendRequest(TileId const & tile_id)
{
  auto tile_request = tile_requests_.find(tile_id);
  if (tile_request == tile_requests_.end()) {
    // the image of this tile is not waited for anymore, so do not retrieve it
    return;
  }

  // see https://foundation.wikimedia.org/wiki/Maps_Terms_of_Use#Using_maps_in_third-party_services
  auto const request_url = QUrl(QString::fromStdString(tileURL(tile_id)));
  QNetworkRequest request(request_url);
  char constexpr agent[] =
    "rviz_satellite " RVIZ_SATELLITE_VERSION " (https://github.com/Kettenhoax/rviz_satellite)";
  request.setHeader(QNetworkRequest::KnownHeaders::UserAgentHeader, agent);
  QVariant variant;
  variant.setValue(tile_id);
  request.setAttribute(
    QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::CacheLoadControl::PreferCache);
  request.setAttribute(QNetworkRequest::User, variant);

  RVIZ_COMMON_LOG_DEBUG_STREAM("Requesting tile " << request_url.toString().toStdString());
  tile_request->second.reply = manager_->get(request);
}

std::shared_future<QImage> TileClient::request_local(TileId const & tile_id)
{
  std::future<QImage> f = std::async(std::launch::async, [tile_id] {
    auto const filename_uri = tileURL(tile_id);

    auto filename = std::regex_replace(filename_uri, std::regex("file://"), "");

    QImageReader reader(QString::fromStdString(filename));

    if (!reader.canRead()) {
      RVIZ_COMMON_LOG_DEBUG_STREAM("Unable to decode image at " << filename);
      return QImage{};
    }

    auto image = flipVertically(reader.read());

    if (image.isNull()) {
      RVIZ_COMMON_LOG_DEBUG_STREAM("QImageReader able to decode but read failed for " << filename);
    }

    return image;
  });

  return std::shared_future<QImage>(std::move(f));
}

void TileClient::request_finished(QNetworkReply * reply)
{
  const QVariant variant = reply->request().attribute(QNetworkRequest::User);
  auto tile_id = variant.value<TileId>();

  const QNetworkReply::NetworkError reply_error = reply->error();
  const bool from_cache = reply->attribute(QNetworkRequest::SourceIsFromCacheAttribute).toBool();

  QImage image;
  std::string error_message;
  if (reply_error != QNetworkReply::NoError) {
    error_message = reply->errorString().toStdString();
  } else {
    // log if tile comes from cache or web
    if (from_cache) {
      RVIZ_COMMON_LOG_DEBUG_STREAM("Loaded tile from cache " << reply->url().toString().toStdString());
    } else {
      RVIZ_COMMON_LOG_DEBUG_STREAM("Loaded tile from web " << reply->url().toString().toStdString());
    }
    QImageReader reader(reply);
    if (!reader.canRead()) {
      error_message = "response does not contain a decodable image";
    } else {
      image = flipVertically(reader.read());
      if (image.isNull()) {
        error_message = "response contains an image which could not be read";
      }
    }
  }
  // the response was read, so the reply is not needed anymore
  reply->deleteLater();

  auto tile_request = tile_requests_.find(tile_id);
  if (tile_request == tile_requests_.end()) {
    // nobody waits for this tile anymore, its request was cancelled while it was running
    RVIZ_COMMON_LOG_DEBUG_STREAM("Result of cancelled tile request for tile '" << tile_id << "'");
    return;
  }

  if (error_message.empty()) {
    // a tile which arrived means the failures are over, whatever they were
    error_.clear();
    tile_request->second.promise->set_value(image);
    tile_requests_.erase(tile_request);
    return;
  }

  ++tile_request->second.failures;
  if (!from_cache && isTemporaryError(reply_error)) {
    // keep the request pending, so the display keeps showing the tiles it has until the image
    // really arrives, and repeat the request with a delay not to overload the failing server
    const int delay_ms = std::min(
      max_retry_delay_ms, initial_retry_delay_ms * (1 << std::min(tile_request->second.failures, 7)));
    error_ = "Request of tile '" + toString(tile_id) + "' failed (" + error_message +
             "), repeating it in " + std::to_string(delay_ms) + " ms";
    RVIZ_COMMON_LOG_WARNING_STREAM(error_);
    QTimer::singleShot(delay_ms, this, [this, tile_id] { sendRequest(tile_id); });
    return;
  }

  error_ = "Request of tile '" + toString(tile_id) + "' failed: " + error_message;
  RVIZ_COMMON_LOG_ERROR_STREAM(error_);
  tile_request->second.promise->set_exception(std::make_exception_ptr(tile_request_error(error_)));
  tile_requests_.erase(tile_request);
}

void TileClient::cancelRequest(const TileId & tile_id)
{
  auto request = tile_requests_.extract(tile_id);
  if (request.empty()) {
    return;
  }
  // the request is not in the map anymore, so its cancellation is not seen by its own report
  cancel(std::move(request.mapped()));
}

void TileClient::cancelRequests()
{
  // take the requests out of the map first, because aborting a reply reports the failure right
  // away and that report must not see the requests which are being cancelled
  auto cancelled = std::exchange(tile_requests_, std::map<TileId, TileRequest>{});
  for (auto & tile_request : cancelled) {
    cancel(std::move(tile_request.second));
  }
  error_.clear();
}

void TileClient::cancel(TileRequest && request)
{
  if (request.reply != nullptr) {
    request.reply->abort();
  }
  if (request.failures > 0) {
    // the reported failure was about the tile which is not requested anymore
    error_.clear();
  }
  try {
    request.promise->set_exception(
      std::make_exception_ptr(tile_request_error("Tile request cancelled")));
  } catch (const std::future_error &) {
    // the image arrived just now, which is fine, nobody is waiting for it anymore
  }
}

}  // namespace rviz_satellite
