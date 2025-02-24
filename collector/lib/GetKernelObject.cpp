#include "GetKernelObject.h"

#include <cstring>
#include <filesystem>

extern "C" {
#include <openssl/sha.h>
#include <sys/stat.h>
}

#include <fstream>

#include "CollectionMethod.h"
#include "FileDownloader.h"
#include "FileSystem.h"
#include "Logging.h"
#include "SysdigService.h"
#include "Utility.h"

namespace collector {

const int kMaxDownloadRetriesTime = 180;
const int kMaxDownloadRetriesInterval = 5;
const int kNumDownloadRetries = kMaxDownloadRetriesTime / kMaxDownloadRetriesInterval;

bool DownloadKernelObjectFromURL(FileDownloader& downloader, const std::string& base_url, const std::string& kernel_module, const std::string& module_version) {
  std::string url(base_url + "/" + module_version + "/" + kernel_module + ".gz");

#ifdef COLLECTOR_APPEND_CID
  // This extra parameter will be dropped by sensor.
  // Its only purpose is to filter alerts coming from our CI.
  url += "?cid=collector";
#endif

  if (!downloader.SetURL(url)) {
    return false;
  }

  CLOG(INFO) << "Attempting to download kernel object from " << downloader.GetEffectiveURL();

  if (!downloader.Download()) {
    return false;
  }

  CLOG(DEBUG) << "Downloaded kernel object from " << url;

  return true;
}

bool DownloadKernelObjectFromHostname(FileDownloader& downloader, const Json::Value& tls_config, const std::string& hostname, const std::string& kernel_module, const std::string& module_version) {
  size_t port_offset = hostname.find(':');
  if (port_offset == std::string::npos) {
    CLOG(WARNING) << "Provided hostname must have a valid port";
    return false;
  }

  const std::string SNI_hostname(GetSNIHostname());
  if (SNI_hostname.find(':') != std::string::npos) {
    CLOG(WARNING) << "SNI hostname must NOT specify a port";
    return false;
  }

  if (tls_config.isNull()) {
    CLOG(WARNING) << "No TLS configuration provided";
    return false;
  }

  if (!downloader.CACert(tls_config["caCertPath"].asCString())) return false;
  if (!downloader.Cert(tls_config["clientCertPath"].asCString())) return false;
  if (!downloader.Key(tls_config["clientKeyPath"].asCString())) return false;

  std::string server_hostname;
  if (hostname.compare(0, port_offset, SNI_hostname) != 0) {
    downloader.SetConnectTo(SNI_hostname, hostname);

    const std::string server_port(hostname.substr(port_offset + 1));
    server_hostname = SNI_hostname + ":" + server_port;
  } else {
    server_hostname = hostname;
  }

  // Attempt to download the kernel object from a given hostname server
  std::string base_url("https://" + server_hostname + "/kernel-objects");
  if (base_url.empty()) return false;

  return DownloadKernelObjectFromURL(downloader, base_url, kernel_module, module_version);
}

bool DownloadKernelObject(const std::string& hostname, const Json::Value& tls_config, const std::string& kernel_module, const std::string& compressed_module_path, bool verbose) {
  FileDownloader downloader;
  if (!downloader.IsReady()) {
    CLOG(WARNING) << "Failed to initialize FileDownloader object";
    return false;
  }

  std::string module_version(GetModuleVersion());
  if (module_version.empty()) {
    CLOG(WARNING) << "/kernel-modules/MODULE_VERSION.txt must exist and not be empty";
    return false;
  }

  downloader.IPResolve(FileDownloader::ANY);
  downloader.SetRetries(kNumDownloadRetries, kMaxDownloadRetriesInterval, kMaxDownloadRetriesTime);
  downloader.SetVerboseMode(verbose);
  downloader.OutputFile(compressed_module_path);
  if (!downloader.SetConnectionTimeout(2)) return false;
  if (!downloader.FollowRedirects(true)) return false;

  if (DownloadKernelObjectFromHostname(downloader, tls_config, hostname, kernel_module, module_version)) {
    return true;
  }

  std::string base_url(GetModuleDownloadBaseURL());
  if (base_url.empty()) {
    return false;
  }

  downloader.ResetCURL();
  downloader.IPResolve(FileDownloader::ANY);
  downloader.SetRetries(kNumDownloadRetries, kMaxDownloadRetriesInterval, kMaxDownloadRetriesTime);
  downloader.SetVerboseMode(verbose);
  downloader.OutputFile(compressed_module_path);
  if (!downloader.SetConnectionTimeout(2)) return false;
  if (!downloader.FollowRedirects(true)) return false;

  if (DownloadKernelObjectFromURL(downloader, base_url, kernel_module, module_version)) {
    return true;
  }
  return false;
}

std::string Sha256HashStream(std::istream& stream) {
  unsigned char hash[SHA256_DIGEST_LENGTH];
  char buffer[4096];
  SHA256_CTX sha256;
  char output[65];

  SHA256_Init(&sha256);
  while (stream) {
    stream.read(buffer, 4096);

    // The stream must have either read correctly or reached EOF
    if (stream.good() || stream.eof()) {
      SHA256_Update(&sha256, buffer, stream.gcount());
    } else {
      CLOG(WARNING) << "Failed to read stream during hash operation";
      return "";
    }
  }

  SHA256_Final(hash, &sha256);

  for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
    sprintf(output + (i * 2), "%02x", hash[i]);
  }

  return std::string{output, 64};
}

std::string Sha256HashFile(const std::filesystem::path driver) {
  std::ifstream file{driver.string(), std::ios::binary};

  if (!file.is_open()) {
    CLOG(WARNING) << "Failed to open " << driver;
    return "";
  }

  return Sha256HashStream(file);
}

bool GetKernelObject(const std::string& hostname, const Json::Value& tls_config, const DriverCandidate& candidate, bool verbose) {
  if (candidate.GetCollectionMethod() == CollectionMethod::CORE_BPF) {
    // for now CO.RE bpf probes are embedded in the collector binary, nothing
    // to do here.
    return true;
  }

  std::string expected_path = candidate.GetPath() + "/" + candidate.GetName();
  std::string expected_path_compressed = expected_path + ".gz";
  std::string module_path = candidate.GetCollectionMethod() == CollectionMethod::EBPF ? SysdigService::kProbePath : SysdigService::kModulePath;
  struct stat sb;

  // first check for an existing compressed kernel object in the
  // kernel-modules directory.
  CLOG(DEBUG) << "Checking for existence of " << expected_path_compressed
              << " and " << expected_path;
  if (stat(expected_path_compressed.c_str(), &sb) == 0) {
    CLOG(INFO) << "Found existing compressed kernel object with sha256 hash: " << Sha256HashFile(expected_path_compressed) << ".";
    if (!GZFileHandle::DecompressFile(expected_path_compressed, module_path)) {
      CLOG(WARNING) << "Failed to decompress " << expected_path_compressed;
      // don't delete the local /kernel-modules gzip file because it is on a read-only file system.
      return false;
    }
  }
  // then check if we have a decompressed object in the kernel-modules
  // directory. If it exists, copy it to modules directory.
  else if (stat(expected_path.c_str(), &sb) == 0) {
    CLOG(DEBUG) << "Found existing kernel object " << expected_path;
    std::ifstream input_file(expected_path, std::ios::binary);
    if (!input_file.is_open()) {
      CLOG(WARNING) << "Failed to open " << expected_path << " - " << StrError();
      return false;
    }

    std::ofstream output_file(module_path, std::ios::binary);
    if (!output_file.is_open()) {
      CLOG(WARNING) << "Failed to open " << module_path << " - " << StrError();
      return false;
    }

    std::copy(std::istreambuf_iterator<char>(input_file),
              std::istreambuf_iterator<char>(),
              std::ostreambuf_iterator<char>(output_file));
  }
  // Otherwise there is no module in local storage, so we should download it.
  else if (candidate.IsDownloadable()) {
    CLOG(INFO) << "Attempting to download " << candidate.GetName();
    std::string downloadPath = module_path + ".gz";
    if (!DownloadKernelObject(hostname, tls_config, candidate.GetName(), downloadPath, verbose)) {
      CLOG(WARNING) << "Unable to download kernel object " << candidate.GetName() << " to " << downloadPath;
      return false;
    }

    CLOG(INFO) << "Downloaded driver with sha256 hash: " << Sha256HashFile(downloadPath);

    if (!GZFileHandle::DecompressFile(downloadPath, module_path)) {
      CLOG(WARNING) << "Failed to decompress downloaded kernel object";
      // If the gzipped file is corrupted, delete it so we don't try to use it
      // next time.
      TryUnlink(downloadPath.c_str());
      return false;
    }
    CLOG(INFO) << "Successfully downloaded and decompressed " << module_path;
  } else {
    CLOG(WARNING) << "Local storage does not contain " << candidate.GetName() << " and the candidate is not downloadable.";
    return false;
  }

  if (chmod(module_path.c_str(), 0444)) {
    CLOG(WARNING) << "Failed to set file permissions for " << module_path << " - " << strerror(errno);
    return false;
  }

  return true;
}
}  // namespace collector
