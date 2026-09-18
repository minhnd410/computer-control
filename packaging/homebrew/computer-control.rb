# Homebrew formula.
#
# Until there is a tagged release this builds from HEAD; once releases exist,
# fill in `url`, `sha256` and `version` and drop the `head` block. Kept in the
# repository rather than a tap so it stays in step with the source.
#
#   brew install --HEAD minhnd410/tap/computer-control
#
class ComputerControl < Formula
  desc "Cross-platform desktop and mobile-simulator automation with an MCP server"
  homepage "https://github.com/minhnd410/computer-control"
  license "MIT"
  head "https://github.com/minhnd410/computer-control.git", branch: "main"

  depends_on "cmake" => :build
  depends_on macos: :monterey   # ScreenCaptureKit needs 12.3+

  on_linux do
    depends_on "libx11"
    depends_on "libxtst"
    depends_on "libxrandr"
    depends_on "libxfixes"
    depends_on "zlib"
  end

  def install
    system "cmake", "-S", ".", "-B", "build",
           "-DCMAKE_BUILD_TYPE=Release",
           # Homebrew relocates and re-signs binaries, which invalidates an
           # ad-hoc signature anyway; the bundle is built separately by anyone
           # who wants a persistent Accessibility grant.
           "-DCC_BUILD_APP_BUNDLE=OFF",
           *std_cmake_args
    system "cmake", "--build", "build"
    system "cmake", "--install", "build"
  end

  def caveats
    on_macos do
      <<~TEXT
        computer-control needs two macOS permissions:
          Accessibility               - input and the element tree
          Screen & System Audio Recording - screenshots

        Run `cc permissions` for the exact state and instructions.

        Note: a CLI launched from a terminal inherits that terminal's grant and
        never appears in System Settings on its own. If element queries come
        back empty, `cc permissions` will explain why and what to add.
      TEXT
    end
  end

  test do
    assert_match "computer-control", shell_output("#{bin}/computer-control-mcp --version")
    assert_match version.to_s, shell_output("#{bin}/cc --version") if build.stable?
    # `displays` needs a window server, which the sandbox lacks, so only the
    # argument-free paths are exercised here.
    system bin/"cc", "--help"
  end
end
