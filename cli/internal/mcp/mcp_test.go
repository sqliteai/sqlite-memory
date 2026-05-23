package mcp

import (
	"bufio"
	"bytes"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"strings"
	"testing"
)

func TestToolNames(t *testing.T) {
	names := ToolNames()
	want := map[string]bool{
		"memory_search":         true,
		"memory_add_file":       true,
		"memory_add_directory":  true,
		"memory_add_text":       true,
		"memory_clear":          true,
		"memory_delete":         true,
		"memory_delete_context": true,
		"memory_reindex":        true,
		"memory_status":         true,
		"memory_get":            true,
		"memory_query":          true,
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
