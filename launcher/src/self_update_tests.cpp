#include "self_update.hpp"

#include <gtest/gtest.h>

namespace chronicles::launcher {
namespace {

distribution::Release launcher_release() {
  distribution::Release release;
  release.channel = "launcher-stable";
  release.version = "0.2.5";
  const std::vector<std::string> required_paths{
      "chronicles-launcher.exe", "launcher-config.json", "WebView2Loader.dll",
      "libcrypto-3-x64.dll", "chronicles-world-tls-local.exe", "chronicles-zone-unlock.exe",
      "resources/legacy-game-runtime-files.json", "prerequisites/MicrosoftEdgeWebview2Setup.exe",
      "web/index.html", "web/app.js", "web/app.css", "web/portal.js",
      "web/assets/fonts/InterVariable.woff2", "web/assets/fonts/Cinzel-Variable.ttf",
      "web/assets/world-harbor.png"};
  for (const auto &path : required_paths) {
    release.files.push_back({path, std::string(64, 'a'), 1});
  }
  release.total_bytes = release.files.size();
  return release;
}

TEST(LauncherSelfUpdate, ComparesNumericVersions) {
  EXPECT_LT(compare_launcher_versions("0.2.9", "0.2.10"), 0);
  EXPECT_EQ(compare_launcher_versions("1.2.3", "1.2.3"), 0);
  EXPECT_GT(compare_launcher_versions("1.0.0", "0.99.99"), 0);
  EXPECT_THROW(compare_launcher_versions("0.02.5", "0.2.5"), std::invalid_argument);
  EXPECT_THROW(compare_launcher_versions("0.2", "0.2.5"), std::invalid_argument);
  EXPECT_THROW(compare_launcher_versions("0.2.5.1", "0.2.5"), std::invalid_argument);
}

TEST(LauncherSelfUpdate, RejectsPlayerDataAndUnknownExecutables) {
  auto release = launcher_release();
  EXPECT_NO_THROW(validate_launcher_release(release));

  release.files.push_back({"runtime/out/launcher/settings.json", std::string(64, 'd'), 1});
  EXPECT_THROW(validate_launcher_release(release), std::invalid_argument);
  release.files.back().path = "arbitrary.exe";
  EXPECT_THROW(validate_launcher_release(release), std::invalid_argument);
  release.files.back().path = "web/../../runtime/settings.json";
  EXPECT_THROW(validate_launcher_release(release), std::invalid_argument);
}

TEST(LauncherSelfUpdate, RequiresPlayerReleaseAndBoundedPayload) {
  auto release = launcher_release();
  release.channel = "stable";
  EXPECT_THROW(validate_launcher_release(release), std::invalid_argument);

  release = launcher_release();
  release.files.erase(release.files.begin());
  EXPECT_THROW(validate_launcher_release(release), std::invalid_argument);

  release = launcher_release();
  release.total_bytes = 256ULL * 1024 * 1024 + 1;
  EXPECT_THROW(validate_launcher_release(release), std::invalid_argument);
}

TEST(LauncherSelfUpdate, RejectsEveryIncompletePlayerRelease) {
  const auto complete = launcher_release();
  for (std::size_t index = 0; index < complete.files.size(); ++index) {
    auto incomplete = complete;
    incomplete.files.erase(incomplete.files.begin() + index);
    EXPECT_THROW(validate_launcher_release(incomplete), std::invalid_argument)
        << complete.files[index].path;
  }
}

} // namespace
} // namespace chronicles::launcher
