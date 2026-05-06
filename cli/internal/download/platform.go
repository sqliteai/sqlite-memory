package download

import (
	"runtime"
	"strings"
)

type Platform struct {
	OS   string
	Arch string
}

func CurrentPlatform() Platform {
	return Platform{OS: runtime.GOOS, Arch: runtime.GOARCH}
}

func (p Platform) OSTokens() []string {
	switch p.OS {
	case "darwin":
		return []string{"darwin", "macos", "mac", "osx"}
	case "windows":
		return []string{"windows", "win"}
	case "linux":
		return []string{"linux"}
	default:
		return []string{strings.ToLower(p.OS)}
	}
}

func (p Platform) ArchTokens() []string {
	switch p.Arch {
	case "amd64":
		return []string{"amd64", "x86_64", "x64", "universal"}
	case "arm64":
		return []string{"arm64", "aarch64", "universal"}
	case "386":
		return []string{"386", "i386", "x86"}
	default:
		return []string{strings.ToLower(p.Arch)}
	}
}

func (p Platform) SharedLibraryExt() string {
	switch p.OS {
	case "darwin":
		return ".dylib"
	case "windows":
		return ".dll"
	default:
		return ".so"
	}
}
