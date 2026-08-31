class Tgcli < Formula
  desc "Telegram command-line client built on TDLib"
  homepage "https://github.com/ei-grad/tgcli"
  url "https://github.com/ei-grad/tgcli/releases/download/v1.0.0/tgcli-1.0.0-macos-universal.tar.gz"
  version "1.0.0"
  sha256 "0000000000000000000000000000000000000000000000000000000000000000"
  license "MIT"

  depends_on :macos
  skip_clean "bin/tgcli"

  def install
    bin.install "tgcli"
    bash_completion.install "share/bash-completion/completions/tgcli"
    fish_completion.install "share/fish/vendor_completions.d/tgcli.fish"
    zsh_completion.install "share/zsh/site-functions/_tgcli"
    man1.install "share/man/man1/tgcli.1"
    (share/"tgcli").install "share/tgcli/public-command-registry.json"
    doc.install "README.md", "THIRD_PARTY_NOTICES.md"
  end

  test do
    output = shell_output("#{bin}/tgcli --json version")
    assert_match '"version":"1.0.0"', output
  end
end
