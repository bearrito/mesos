#include <unistd.h>

#include <sys/stat.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <boost/shared_array.hpp>

#include <process/dispatch.hpp>
#include <process/future.hpp>
#include <process/http.hpp>
#include <process/io.hpp>
#include <process/mime.hpp>
#include <process/process.hpp>

#include <stout/error.hpp>
#include <stout/hashmap.hpp>
#include <stout/json.hpp>
#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/numify.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/result.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include "files/files.hpp"

#include "logging/logging.hpp"

using namespace process;

using process::wait; // Necessary on some OS's to disambiguate.

using process::http::BadRequest;
using process::http::InternalServerError;
using process::http::NotFound;
using process::http::OK;
using process::http::Response;
using process::http::Request;

using std::map;
using std::string;
using std::vector;

namespace mesos {
namespace internal {

class FilesProcess : public Process<FilesProcess>
{
public:
  FilesProcess();

  // Files implementation.
  Future<Nothing> attach(const string& path, const string& name);
  void detach(const string& name);

protected:
  virtual void initialize();

private:
  // Resolves the virtual path to an actual path.
  // Returns the actual path if found.
  // Returns None if the file is not found.
  // Returns Error if we find the file but it cannot be resolved or it breaks
  // out of the chroot.
  Result<std::string> resolve(const string& path);

  // HTTP endpoints.

  // Returns a file listing for a directory.
  // Requests have the following parameters:
  //   path: The directory to browse. Required.
  // The response will contain a list of JSON files and directories contained
  // in the path (see files::jsonFileInfo for the format).
  Future<Response> browse(const Request& request);

  // Reads data from a file at a given offset and for a given length.
  // See the jquery pailer for the expected behavior.
  Future<Response> read(const Request& request);

  // Returns the raw file contents for a given path.
  // Requests have the following parameters:
  //   path: The directory to browse. Required.
  Future<Response> download(const Request& request);

  // Returns the internal virtual path mapping.
  Future<Response> debug(const Request& request);

  hashmap<string, string> paths;
};


FilesProcess::FilesProcess()
  : ProcessBase("files")
{}


void FilesProcess::initialize()
{
  route("/browse.json", &FilesProcess::browse);
  route("/read.json", &FilesProcess::read);
  route("/download.json", &FilesProcess::download);
  route("/debug.json", &FilesProcess::debug);
}


Future<Nothing> FilesProcess::attach(const string& path, const string& name)
{
  Try<string> result = os::realpath(path);

  if (result.isError()) {
    return Future<Nothing>::failed(
        "Failed to get realpath of '" + path + "': " + result.error());
  }

  // Make sure we have permissions to read the file/dir.
  Try<bool> access = os::access(result.get(), R_OK);

  if (access.isError() || !access.get()) {
    return Future<Nothing>::failed("Failed to access '" + path + "': " +
        (access.isError() ? access.error() : "Access denied"));
  }

  // To simplify the read/browse logic, strip any trailing / from the name.
  string cleanedName = strings::remove(name, "/", strings::SUFFIX);

  // TODO(bmahler): Do we want to always wipe out the previous path?
  paths[cleanedName] = result.get();

  return Nothing();
}


void FilesProcess::detach(const string& name)
{
  paths.erase(name);
}


Future<Response> FilesProcess::browse(const Request& request)
{
  Option<string> path = request.query.get("path");

  if (!path.isSome() || path.get().empty()) {
    return BadRequest("Expecting 'path=value' in query.\n");
  }

  Result<string> resolvedPath = resolve(path.get());

  if (resolvedPath.isError()) {
    return InternalServerError(resolvedPath.error() + ".\n");
  } else if (resolvedPath.isNone()) {
    return NotFound();
  }

  // The result will be a sorted (on path) array of files and dirs:
  // [{"name": "README", "path": "dir/README" "dir":False, "size":42}, ...]
  map<string, JSON::Object> files;
  foreach (const string& filename, os::ls(resolvedPath.get())) {
    struct stat s;
    string fullPath = path::join(resolvedPath.get(), filename);

    if (stat(fullPath.c_str(), &s) < 0) {
      PLOG(WARNING) << "Found " << fullPath << " in ls but stat failed";
      continue;
    }

    files[fullPath] = jsonFileInfo(path::join(path.get(), filename), s);
  }

  JSON::Array listing;
  foreachvalue(const JSON::Object& file, files) {
    listing.values.push_back(file);
  }

  return OK(listing, request.query.get("jsonp"));
}


// TODO(benh): Remove 'const &' from size after fixing libprocess.
Future<Response> _read(int fd,
                       const size_t& size,
                       off_t offset,
                       const boost::shared_array<char>& data,
                       const Option<string>& jsonp) {
  JSON::Object object;

  object.values["offset"] = offset;
  object.values["data"] = string(data.get(), size);

  os::close(fd);

  return OK(object, jsonp);
}


Future<Response> FilesProcess::read(const Request& request)
{
  Option<string> path = request.query.get("path");

  if (!path.isSome() || path.get().empty()) {
    return BadRequest("Expecting 'path=value' in query.\n");
  }

  off_t offset = -1;

  if (request.query.get("offset").isSome()) {
    Try<off_t> result = numify<off_t>(request.query.get("offset").get());
    if (result.isError()) {
      return BadRequest("Failed to parse offset: " + result.error() + ".\n");
    }
    offset = result.get();
  }

  ssize_t length = -1;

  if (request.query.get("length").isSome()) {
    Try<ssize_t> result = numify<ssize_t>(request.query.get("length").get());
    if (result.isError()) {
      return BadRequest("Failed to parse length: " + result.error() + ".\n");
    }
    length = result.get();
  }

  Result<string> resolvedPath = resolve(path.get());

  if (resolvedPath.isError()) {
    return BadRequest(resolvedPath.error() + ".\n");
  } else if (!resolvedPath.isSome()) {
    return NotFound();
  }

  // Don't read directories.
  if (os::isdir(resolvedPath.get())) {
    return BadRequest("Cannot read a directory.\n");
  }

  // TODO(benh): Cache file descriptors so we aren't constantly
  // opening them and paging the data in from disk.
  Try<int> fd = os::open(resolvedPath.get(), O_RDONLY);

  if (fd.isError()) {
    string error = strings::format("Failed to open file at '%s': %s",
        resolvedPath.get(), fd.error()).get();
    LOG(WARNING) << error;
    return InternalServerError(error + ".\n");
  }

  off_t size = lseek(fd.get(), 0, SEEK_END);

  if (size == -1) {
    string error = strings::format("Failed to open file at '%s': %s",
        resolvedPath.get(), strerror(errno)).get();
    LOG(WARNING) << error;
    os::close(fd.get());
    return InternalServerError(error + ".\n");
  }

  if (offset == -1) {
    offset = size;
  }

  if (length == -1) {
    length = size - offset;
  }

  // Cap the read length at 16 pages.
  length = std::min(length, sysconf(_SC_PAGE_SIZE) * 16);

  if (offset >= size) {
    os::close(fd.get());

    JSON::Object object;
    object.values["offset"] = size;
    object.values["data"] = "";
    return OK(object, request.query.get("jsonp"));
  }

  // Seek to the offset we want to read from.
  if (lseek(fd.get(), offset, SEEK_SET) == -1) {
    string error = strings::format("Failed to seek file at '%s': %s",
        resolvedPath.get(), strerror(errno)).get();
    LOG(WARNING) << error;
    os::close(fd.get());
    return InternalServerError(error);
  }

  Try<Nothing> nonblock = os::nonblock(fd.get());
  if (nonblock.isError()) {
    string error =
        "Failed to set file descriptor nonblocking: " + nonblock.error();
    LOG(WARNING) << error;
    return InternalServerError(error);
  }

  // Read 'length' bytes (or to EOF).
  boost::shared_array<char> data(new char[length]);

  return io::read(fd.get(), data.get(), static_cast<size_t>(length))
    .then(lambda::bind(
        _read,
        fd.get(),
        lambda::_1,
        offset,
        data,
        request.query.get("jsonp")));
}


Future<Response> FilesProcess::download(const Request& request)
{
  Option<string> path = request.query.get("path");

  if (!path.isSome() || path.get().empty()) {
    return BadRequest("Expecting 'path=value' in query.\n");
  }

  Result<string> resolvedPath = resolve(path.get());

  if (resolvedPath.isError()) {
    return BadRequest(resolvedPath.error() + ".\n");
  } else if (!resolvedPath.isSome()) {
    return NotFound();
  }

  // Don't download directories.
  if (os::isdir(resolvedPath.get())) {
    return BadRequest("Cannot download a directory.\n");
  }

  Try<string> basename = os::basename(resolvedPath.get());
  if (basename.isError()) {
    LOG(ERROR) << basename.error();
    return InternalServerError(basename.error() + ".\n");
  }

  OK response;
  response.type = response.PATH;
  response.path = resolvedPath.get();
  response.headers["Content-Type"] = "application/octet-stream";
  response.headers["Content-Disposition"] =
    strings::format("attachment; filename=%s", basename.get()).get();

  // Attempt to detect the mime type.
  size_t index = basename.get().find_last_of('.');
  if (index != string::npos) {
    string extension = basename.get().substr(index);
    if (mime::types.count(extension) > 0) {
      response.headers["Content-Type"] = mime::types[extension];
    }
  }

  return response;
}


Future<Response> FilesProcess::debug(const Request& request)
{
  JSON::Object object;
  foreachpair(const string& name, const string& path, paths) {
    object.values[name] = path;
  }
  return OK(object, request.query.get("jsonp"));
}


Result<string> FilesProcess::resolve(const string& path)
{
  // Suppose we have: /1/2/hello_world.txt
  // And we attach: /1/2 as /sandbox
  // Then this function would resolve the following virtual path
  // into the actual path:
  // input: /sandbox/hello_world.txt
  // output: /1/2/hello_world.txt
  //
  // Try and see if this path has been attached. We check for the
  // longest possible prefix match and if found append any suffix to
  // the attached path (provided the path is to a directory).
  vector<string> tokens = strings::split(
      strings::remove(path, "/", strings::SUFFIX), "/");

  string suffix;
  while (!tokens.empty()) {
    string prefix = path::join(tokens);

    if (!paths.contains(prefix)) {
      if (suffix.empty()) {
        suffix = tokens.back();
      } else {
        suffix = path::join(tokens.back(), suffix);
      }

      tokens.pop_back();
      continue;
    }

    // Determine the final path: if it's a directory, append the
    // suffix, if it's not a directory and there is a suffix, return
    // 'Not Found'.
    string path = paths[prefix];
    if (os::isdir(path)) {
      path = path::join(path, suffix);

      // Canonicalize the absolute path.
      Try<string> realpath = os::realpath(path);
      if (realpath.isError()) {
        return Error(
            "Failed to determine canonical path of '" + path +
            "': " + realpath.error());
      }

      // Make sure the canonicalized absolute path is accessible
      // (i.e., not outside the "chroot").
      if (!strings::startsWith(realpath.get(), paths[prefix])) {
        return Error("'" + path + "' is inaccessible");
      }

      path = realpath.get();
    } else if (suffix != "") {
      // Request is assuming attached path is a directory, but it is
      // not! Rather than 'Bad Request', treat this as 'Not Found'.
      return None();
    }

    return path;
  }

  return None();
}


Files::Files()
{
  process = new FilesProcess();
  spawn(process);
}


Files::~Files()
{
  terminate(process);
  wait(process);
  delete process;
}


Future<Nothing> Files::attach(const string& path, const string& name)
{
  return dispatch(process, &FilesProcess::attach, path, name);
}


void Files::detach(const string& name)
{
  dispatch(process, &FilesProcess::detach, name);
}

} // namespace internal {
} // namespace mesos {
