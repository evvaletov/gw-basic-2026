class Gwbasic < Formula
  desc "Line-numbered BASIC interpreter and ahead-of-time native compiler"
  homepage "https://github.com/evvaletov/gw-basic-2026"
  url "https://github.com/evvaletov/gw-basic-2026/releases/download/v0.18.3/gwbasic-0.18.3-source.tar.gz"
  version "0.18.3"
  sha256 "5c4402a784e839bec1db4c35aa0c3f6c6d76d26ac87871bd2fa896d35ba23bdd"
  license "MIT"

  depends_on "cmake" => :build

  def install
    # This release auto-detects PulseAudio. Build without that optional backend.
    system "cmake", "-S", ".", "-B", "build", *std_cmake_args,
                    "-DPKG_CONFIG_EXECUTABLE=/usr/bin/false"
    system "cmake", "--build", "build"
    bin.install "build/gwbasic"
    libexec.install "build/gwbasic-compile"
    (libexec/"build").install "build/libgwrt.a"
    (libexec/"include").install Dir["include/*.h"]
    (bin/"gwbasic-compile").write <<~SH
      #!/bin/sh
      exec "#{libexec}/gwbasic-compile" --runtime "#{libexec}" "$@"
    SH
    (bin/"gwbasic-compile").chmod 0755
    (pkgshare/"examples").install "examples/squares.bas", "examples/circles.bas"
  end

  test do
    (testpath/"squares.bas").write <<~BASIC
      10 FOR N=1 TO 3
      20 PRINT N*N
      30 NEXT N
      40 END
    BASIC
    output = shell_output("#{bin}/gwbasic squares.bas < /dev/null")
    assert_equal %w[1 4 9], output.split
    system bin/"gwbasic-compile", "squares.bas", "-c"
    assert_equal output, shell_output("./squares")
  end
end
