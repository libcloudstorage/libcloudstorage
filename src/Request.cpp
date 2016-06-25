/*****************************************************************************
 * Request.cpp : Request implementation
 *
 *****************************************************************************
 * Copyright (C) 2016-2016 VideoLAN
 *
 * Authors: Paweł Wegner <pawel.wegner95@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

#include "Request.h"

#include "CloudProvider.h"
#include "Utility.h"

namespace cloudstorage {

namespace {

class HttpCallback : public HttpRequest::ICallback {
 public:
  HttpCallback(std::atomic_bool& is_cancelled) : is_cancelled_(is_cancelled) {}

  bool abort() { return is_cancelled_; }

  void progressDownload(uint, uint) {}

  void progressUpload(uint, uint) {}

  void receivedHttpCode(int) {}

  void receivedContentLength(int) {}

 private:
  std::atomic_bool& is_cancelled_;
};

}  // namespace

Request::Request(std::shared_ptr<CloudProvider> provider)
    : provider_(provider), is_cancelled_(false) {}

void Request::cancel() {
  set_cancelled(true);
  finish();
}

std::unique_ptr<HttpCallback> Request::httpCallback() {
  return make_unique<HttpCallback>(is_cancelled_);
}

ListDirectoryRequest::ListDirectoryRequest(std::shared_ptr<CloudProvider> p,
                                           IItem::Pointer directory,
                                           ICallback::Pointer callback)
    : Request(p),
      directory_(std::move(directory)),
      callback_(std::move(callback)) {
  result_ = std::async(std::launch::async, [this]() {
    provider()->waitForAuthorized();
    std::vector<IItem::Pointer> result;
    HttpRequest::Pointer r =
        provider()->listDirectoryRequest(*directory_, input_stream());
    do {
      std::stringstream output_stream;
      std::string backup_data = input_stream().str();
      provider()->authorizeRequest(*r);
      int code =
          r->send(input_stream(), output_stream, nullptr, httpCallback());
      if (HttpRequest::isSuccess(code)) {
        for (auto& t : provider()->listDirectoryResponse(output_stream, r,
                                                         input_stream())) {
          if (callback_) callback_->receivedItem(t);
          result.push_back(t);
        }
      } else if (HttpRequest::isClientError(code)) {
        if (!provider()->authorize()) throw AuthorizationException();
        input_stream() = std::stringstream();
        input_stream() << backup_data;
      } else {
        r = nullptr;
      }
    } while (r);
    return result;
  });
}

ListDirectoryRequest::~ListDirectoryRequest() { cancel(); }

void ListDirectoryRequest::finish() {
  if (result_.valid()) result_.wait();
}

std::vector<IItem::Pointer> ListDirectoryRequest::result() {
  std::shared_future<std::vector<IItem::Pointer>> future = result_;
  if (!future.valid()) throw std::logic_error("Future invalid.");
  return future.get();
}

GetItemRequest::GetItemRequest(std::shared_ptr<CloudProvider> p,
                               const std::string& path,
                               std::function<void(IItem::Pointer)> callback)
    : Request(p), path_(path), callback_(callback) {
  result_ = std::async(std::launch::async, [this]() -> IItem::Pointer {
    if (path_.empty() || path_.front() != '/') return nullptr;
    IItem::Pointer node = provider()->rootDirectory();
    std::stringstream stream(path_.substr(1));
    std::string token;
    while (std::getline(stream, token, '/')) {
      if (!node || !node->is_directory()) return nullptr;
      std::shared_future<std::vector<IItem::Pointer>> current_directory;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        if (is_cancelled()) return nullptr;
        current_request_ =
            provider()->listDirectoryAsync(std::move(node), nullptr);
        current_directory = current_request_->result_;
      }
      node = getItem(current_directory.get(), token);
    }
    if (callback_) callback_(node);
    return node;
  });
}

GetItemRequest::~GetItemRequest() { cancel(); }

void GetItemRequest::finish() {
  if (result_.valid()) result_.wait();
}

void GetItemRequest::cancel() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    set_cancelled(true);
    if (current_request_) current_request_->cancel();
  }
  finish();
}

IItem::Pointer GetItemRequest::result() {
  std::shared_future<IItem::Pointer> future = result_;
  if (!future.valid()) throw std::logic_error("Future invalid.");
  return future.get();
}

IItem::Pointer GetItemRequest::getItem(const std::vector<IItem::Pointer>& items,
                                       const std::string& name) const {
  for (const IItem::Pointer& i : items)
    if (i->filename() == name) return i;
  return nullptr;
}

DownloadFileRequest::DownloadFileRequest(std::shared_ptr<CloudProvider> p,
                                         IItem::Pointer file,
                                         ICallback::Pointer callback)
    : Request(p), file_(std::move(file)), stream_wrapper_(std::move(callback)) {
  function_ = std::async(std::launch::async, [this]() {
    provider()->waitForAuthorized();
    int code = download();
    if (HttpRequest::isClientError(code)) {
      if (stream_wrapper_.callback_) stream_wrapper_.callback_->reset();
      if (!provider()->authorize()) throw AuthorizationException();
      code = download();
      if (!HttpRequest::isSuccess(code))
        throw std::logic_error("Invalid download request.");
    }
    if (HttpRequest::isSuccess(code) && stream_wrapper_.callback_)
      stream_wrapper_.callback_->done();
  });
}

DownloadFileRequest::~DownloadFileRequest() { cancel(); }

void DownloadFileRequest::finish() {
  if (function_.valid()) function_.wait();
}

int DownloadFileRequest::download() {
  std::stringstream stream;
  HttpRequest::Pointer request =
      provider()->downloadFileRequest(*file_, stream);
  provider()->authorizeRequest(*request);
  std::ostream response_stream(&stream_wrapper_);
  return request->send(stream, response_stream, nullptr, httpCallback());
}

DownloadFileRequest::DownloadStreamWrapper::DownloadStreamWrapper(
    DownloadFileRequest::ICallback::Pointer callback)
    : callback_(std::move(callback)) {}

std::streamsize DownloadFileRequest::DownloadStreamWrapper::xsputn(
    const char_type* data, std::streamsize length) {
  if (callback_) callback_->receivedData(data, length);
  return length;
}

UploadFileRequest::UploadFileRequest(
    std::shared_ptr<CloudProvider> p, IItem::Pointer directory,
    const std::string& filename, UploadFileRequest::ICallback::Pointer callback)
    : Request(p),
      directory_(std::move(directory)),
      filename_(filename),
      stream_wrapper_(std::move(callback)) {
  function_ = std::async(std::launch::async, [this]() {
    provider()->waitForAuthorized();
    int code = upload();
    if (HttpRequest::isClientError(code)) {
      stream_wrapper_.callback_->reset();
      if (!provider()->authorize()) throw AuthorizationException();
      code = upload();
      if (!HttpRequest::isSuccess(code))
        throw std::logic_error("Invalid upload request.");
    }
    if (HttpRequest::isSuccess(code)) stream_wrapper_.callback_->done();
  });
}

UploadFileRequest::~UploadFileRequest() { cancel(); }

void UploadFileRequest::finish() {
  if (function_.valid()) function_.wait();
}

int UploadFileRequest::upload() {
  std::istream upload_data_stream(&stream_wrapper_);
  std::stringstream request_data;
  HttpRequest::Pointer request = provider()->uploadFileRequest(
      *directory_, filename_, upload_data_stream, request_data);
  provider()->authorizeRequest(*request);
  std::stringstream response;
  return request->send(request_data, response, nullptr, httpCallback());
}

UploadFileRequest::UploadStreamWrapper::UploadStreamWrapper(
    UploadFileRequest::ICallback::Pointer callback)
    : callback_(std::move(callback)) {}

std::streambuf::int_type UploadFileRequest::UploadStreamWrapper::underflow() {
  uint size = callback_->putData(buffer_, BUFFER_SIZE);
  if (gptr() == egptr()) setg(buffer_, buffer_, buffer_ + size);
  return gptr() == egptr() ? std::char_traits<char>::eof()
                           : std::char_traits<char>::to_int_type(*gptr());
}

}  // namespace cloudstorage
