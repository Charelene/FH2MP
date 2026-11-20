package lsp

import (
	"errors"
	"testing"

	"fh2mp/internal/auth"
	"fh2mp/internal/pyjson"
)

// The 1.5.0 cutover: with minLauncher 2.1.0 (Encore 2.1.0) an FH2MP Launcher 1.4.x is refused at sign-in with 426 launcher_too_old,
// Encore 2.1.0 and later and our debug builds are let through, a release candidate of 2.1.0 is not, and clients that do not announce
// themselves as launchers (test tools) are not held to it.
func TestRequireLauncherAtTheEncoreCutover(t *testing.T) {
	l := &Launcher{Config: &LauncherConfig{data: defaultConfig(), Overlay: pyjson.Obj{{K: "minLauncher", V: "2.1.0"}}}}
	for client, refused := range map[string]bool{
		"launcher/1.4.8 windows":       true,
		"launcher/1.4.1 linux":         true,
		"launcher/2.0.1 windows":       true,
		"launcher/2.1.0-rc1 windows":   true,
		"launcher/2.1.0 windows":       false,
		"launcher/2.1.0 linux":         false,
		"launcher/2.1.1 windows":       false,
		"launcher/2.10.0 windows":      false,
		"launcher/2.1.0-debug windows": false,
		"launcher/1.4.8-debug windows": false,
		"rig-signin":                   false,
		"":                             false,
	} {
		err := l.requireLauncher(client)
		var ae *auth.Error
		if !refused {
			if err != nil {
				t.Errorf("%q refused: %v", client, err)
			}
			continue
		}
		if !errors.As(err, &ae) || ae.Status != 426 || ae.Code != "launcher_too_old" {
			t.Errorf("%q: %v, want 426 launcher_too_old", client, err)
		}
	}
	err := l.requireLauncher("launcher/1.4.8 windows")
	if err == nil || err.Error() != "This launcher (1.4.8) is out of date. Update it, or download Encore, the FH2MP launcher, from https://forza.scuffed.af." {
		t.Fatalf("message: %v", err)
	}

	// before the cutover's minimum is set (the defaults: 0.0.0) nothing is refused
	l = &Launcher{Config: &LauncherConfig{data: defaultConfig()}}
	if err := l.requireLauncher("launcher/1.4.8 windows"); err != nil {
		t.Fatalf("minLauncher 0.0.0: %v", err)
	}
}
