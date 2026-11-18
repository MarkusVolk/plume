#pragma once

#include <filesystem>
#include <functional>
#include <gio/gio.h>
#include <memory>
#include <string>
#include <vector>

namespace plume {
  // Open and Save dialogs through xdg-desktop-portal, so the desktop's file chooser is used.
  class FilePortal {
  public:
    // The chosen paths; empty when the dialog was cancelled or no portal answered.
    using Callback = std::function<void(std::vector<std::filesystem::path>)>;

    FilePortal();
    ~FilePortal();
    FilePortal(const FilePortal&) = delete;
    FilePortal& operator=(const FilePortal&) = delete;

    bool available() const noexcept { return m_connection != nullptr; }
    // `parent` is the window's portal handle ("wayland:…"), empty for none.
    void openFiles(const std::string& parent, const std::filesystem::path& folder, Callback callback);
    void saveFile(
        const std::string& parent, const std::filesystem::path& folder, const std::string& name, Callback callback
    );

  private:
    struct Request;
    void call(const char* method, const std::string& parent, const char* title, GVariantBuilder& options, Callback callback);
    static void response(
        GDBusConnection* connection, const char* sender, const char* path, const char* interface, const char* signal,
        GVariant* parameters, gpointer data
    );
    void finish(Request* request, std::vector<std::filesystem::path> paths);

    GDBusConnection* m_connection = nullptr;
    std::vector<std::unique_ptr<Request>> m_requests;
    unsigned m_counter = 0;
  };
} // namespace plume
