package mcp

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestToolNames(t *testing.T) {
	names := ToolNames()
	want := map[string]bool{
		"memory_search":         true,
		"memory_add_file":       true,
		"memory_add_content":    true,
		"memory_add_directory":  true,
		"memory_add_text":       true,
		"memory_clear":          true,
		"memory_delete":         true,
		"memory_delete_context": true,
		"memory_reindex":        true,
		"memory_status":         true,
	}
	for _, name := range names {
		delete(want, name)
	}
	if len(want) != 0 {
		t.Fatalf("missing tools: %#v", want)
	}
}

func TestToolsListMapping(t *testing.T) {
	res, ok := (Server{}).Handle(context.Background(), request{JSONRPC: "2.0", ID: 1, Method: "tools/list"})
	if !ok {
		t.Fatal("expected response")
	}
	if res.Error != nil {
		t.Fatalf("unexpected error: %#v", res.Error)
	}
	result := res.Result.(map[string]any)
	tools := result["tools"].([]map[string]any)
	if len(tools) != len(ToolNames()) {
		t.Fatalf("tool count = %d", len(tools))
	}
	for _, tool := range tools {
		if _, ok := tool["inputSchema"].(map[string]any); !ok {
			t.Fatalf("tool lacks inputSchema: %#v", tool)
		}
	}
}

func TestInitializeAdvertisesToolsCapability(t *testing.T) {
	res, ok := (Server{}).Handle(context.Background(), request{JSONRPC: "2.0", ID: 1, Method: "initialize"})
	if !ok {
		t.Fatal("expected response")
	}
	result := res.Result.(map[string]any)
	capabilities := result["capabilities"].(map[string]any)
	if _, ok := capabilities["tools"].(map[string]any); !ok {
		t.Fatalf("tools capability missing: %#v", capabilities)
	}
}

func TestHandleNotificationWithoutResponse(t *testing.T) {
	_, ok := (Server{}).Handle(context.Background(), request{JSONRPC: "2.0", Method: "notifications/initialized"})
	if ok {
		t.Fatal("notification returned a response")
	}
}

func TestServeStdioUsesContentLengthFraming(t *testing.T) {
	req := []byte(`{"jsonrpc":"2.0","id":1,"method":"initialize"}`)
	in := strings.NewReader(fmt.Sprintf("Content-Length: %d\r\n\r\n%s", len(req), req))
	var out bytes.Buffer
	if err := (Server{}).ServeStdio(context.Background(), in, &out); err != nil {
		t.Fatal(err)
	}
	msg, err := readFramedMessage(bufioReader(&out))
	if err != nil {
		t.Fatal(err)
	}
	var res response
	if err := json.Unmarshal(msg, &res); err != nil {
		t.Fatal(err)
	}
	if res.ID != float64(1) || res.Error != nil {
		t.Fatalf("response = %#v", res)
	}
}

func bufioReader(r io.Reader) *bufio.Reader {
	return bufio.NewReader(r)
}

func TestOriginAllowed(t *testing.T) {
	cases := []struct {
		origin string
		want   bool
	}{
		{"", true}, // real MCP clients and curl send no Origin
		{"http://127.0.0.1:8765", true},
		{"http://localhost:8765", true},
		{"http://[::1]:8765", true},
		{"https://localhost", true},
		{"http://evil.example", false},  // a page the user happens to visit
		{"https://evil.example", false}, // ...over TLS
		{"http://127.0.0.1.evil.test", false},
		{"http://localhost.evil.test", false},
		{"null", false}, // sandboxed iframe / some redirects
		{"://", false},  // unparseable
		// userinfo must not be mistaken for the host
		{"http://localhost@evil.example", false},
		{"http://127.0.0.1@evil.example/x", false},
		// a host that merely contains a loopback name
		{"http://myLocalhost", false},
		{"http://localhost.", false},
		{"http://127.0.0.2", false},
		// non-http schemes still carry a host that must match
		{"file://localhost", true},
		{"https://evil.example:80", false},
	}
	for _, c := range cases {
		r := httptest.NewRequest(http.MethodPost, "/mcp", strings.NewReader("{}"))
		if c.origin != "" {
			r.Header.Set("Origin", c.origin)
		}
		if got := originAllowed(r); got != c.want {
			t.Errorf("originAllowed(Origin: %q) = %v, want %v", c.origin, got, c.want)
		}
	}
}

func TestHTTPHandlerRejectsForeignOrigin(t *testing.T) {
	srv := httptest.NewServer(Server{}.httpHandler())
	defer srv.Close()

	body := `{"jsonrpc":"2.0","id":1,"method":"tools/list"}`

	// a browser on an attacker's page: must be refused before any tool runs
	req, err := http.NewRequest(http.MethodPost, srv.URL+"/mcp", strings.NewReader(body))
	if err != nil {
		t.Fatal(err)
	}
	req.Header.Set("Origin", "http://evil.example")
	res, err := srv.Client().Do(req)
	if err != nil {
		t.Fatal(err)
	}
	defer res.Body.Close()
	if res.StatusCode != http.StatusForbidden {
		t.Fatalf("cross-origin request: status = %d, want %d", res.StatusCode, http.StatusForbidden)
	}

	// a non-browser client (no Origin) must still work
	res2, err := srv.Client().Post(srv.URL+"/mcp", "application/json", strings.NewReader(body))
	if err != nil {
		t.Fatal(err)
	}
	defer res2.Body.Close()
	if res2.StatusCode != http.StatusOK {
		t.Fatalf("no-Origin request: status = %d, want %d", res2.StatusCode, http.StatusOK)
	}
	var parsed response
	if err := json.NewDecoder(res2.Body).Decode(&parsed); err != nil {
		t.Fatal(err)
	}
	if parsed.Result == nil {
		t.Fatal("expected a tools/list result for a request without Origin")
	}
}
