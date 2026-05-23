package mcp

import (
	"bufio"
	"context"
	"database/sql"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"strconv"
	"strings"

	"github.com/sqliteai/sqlite-memory/cli/internal/memory"
)

type Server struct {
	DB *sql.DB
}

type request struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      any             `json:"id,omitempty"`
	Method  string          `json:"method"`
	Params  json.RawMessage `json:"params,omitempty"`
}

type response struct {
	JSONRPC string `json:"jsonrpc"`
	ID      any    `json:"id,omitempty"`
	Result  any    `json:"result,omitempty"`
	Error   any    `json:"error,omitempty"`
}

func ToolNames() []string {
	return []string{
		"memory_search",
		"memory_add_file",
		"memory_add_directory",
		"memory_add_text",
		"memory_clear",
		"memory_delete",
		"memory_delete_context",
		"memory_reindex",
		"memory_status",
		"memory_get",
		"memory_query",
	}
}

func (s Server) ServeStdio(ctx context.Context, in io.Reader, out io.Writer) error {
	reader := bufio.NewReader(in)
	for {
		msg, err := readFramedMessage(reader)
		if err == io.EOF {
			return nil
		}
		if err != nil {
			return writeFramedMessage(out, response{JSONRPC: "2.0", Error: errObj(-32700, err.Error())})
		}
		var req request
		if err := json.Unmarshal(msg, &req); err != nil {
			if err := writeFramedMessage(out, response{JSONRPC: "2.0", Error: errObj(-32700, err.Error())}); err != nil {
				return err
			}
			continue
		}
		res, ok := s.Handle(ctx, req)
		if !ok {
			continue
		}
		if err := writeFramedMessage(out, res); err != nil {
			return err
		}
	}
}

func (s Server) ServeHTTP(ctx context.Context, addr string) error {
	mux := http.NewServeMux()
	mux.HandleFunc("/mcp", func(w http.ResponseWriter, r *http.Request) {
		defer r.Body.Close()
		var req request
		if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
			writeJSON(w, response{JSONRPC: "2.0", Error: errObj(-32700, err.Error())})
			return
		}
		res, ok := s.Handle(r.Context(), req)
		if !ok {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		writeJSON(w, res)
	})
	server := &http.Server{Addr: addr, Handler: mux}
	go func() {
		<-ctx.Done()
		_ = server.Shutdown(context.Background())
	}()
	return server.ListenAndServe()
}

func (s Server) Handle(ctx context.Context, req request) (response, bool) {
	if req.ID == nil {
		return response{}, false
	}
	switch req.Method {
	case "initialize":
		return ok(req.ID, map[string]any{
			"protocolVersion": "2024-11-05",
			"serverInfo":      map[string]any{"name": "sqlmem", "version": "0.1.0"},
			"capabilities":    map[string]any{"tools": map[string]any{}},
		}), true
	case "tools/list":
		return ok(req.ID, map[string]any{"tools": tools()}), true
	case "tools/call":
		var params struct {
			Name      string         `json:"name"`
			Arguments map[string]any `json:"arguments"`
		}
		if err := json.Unmarshal(req.Params, &params); err != nil {
			return fail(req.ID, -32602, err.Error()), true
		}
		result, err := s.callTool(ctx, params.Name, params.Arguments)
		if err != nil {
			return fail(req.ID, -32000, err.Error()), true
		}
		return ok(req.ID, map[string]any{"content": []map[string]string{{"type": "text", "text": result}}}), true
	default:
		return fail(req.ID, -32601, "method not found"), true
	}
}

func tools() []map[string]any {
	return []map[string]any{
		tool("memory_search", map[string]any{
			"query": stringSchema("Search query"),
			"limit": intSchema("Maximum number of results"),
		}, []string{"query"}),
		tool("memory_add_file", map[string]any{
			"path":    stringSchema("File path"),
			"context": stringSchema("Context label"),
		}, []string{"path"}),
		tool("memory_add_directory", map[string]any{
			"path":    stringSchema("Directory path"),
			"context": stringSchema("Context label"),
		}, []string{"path"}),
		tool("memory_add_text", map[string]any{
			"text":    stringSchema("Text content"),
			"context": stringSchema("Context label"),
		}, []string{"text"}),
		tool("memory_clear", map[string]any{}, nil),
		tool("memory_delete", map[string]any{
			"hash": stringSchema("Content hash"),
		}, []string{"hash"}),
		tool("memory_delete_context", map[string]any{
			"context": stringSchema("Context label"),
		}, []string{"context"}),
		tool("memory_reindex", map[string]any{}, nil),
		tool("memory_status", map[string]any{}, nil),
		tool("memory_get", map[string]any{
			"hash": stringSchema("Content hash from a memory_search result"),
		}, []string{"hash"}),
		tool("memory_query", map[string]any{
			"query": stringSchema("Read-only SELECT statement to run against the memory database"),
		}, []string{"query"}),
	}
}

func tool(name string, properties map[string]any, required []string) map[string]any {
	schema := map[string]any{
		"type":       "object",
		"properties": properties,
	}
	if len(required) > 0 {
		schema["required"] = required
	}
	return map[string]any{
		"name":        name,
		"description": name,
		"inputSchema": schema,
	}
}

func stringSchema(description string) map[string]string {
	return map[string]string{"type": "string", "description": description}
}

func intSchema(description string) map[string]string {
	return map[string]string{"type": "integer", "description": description}
}

func (s Server) callTool(ctx context.Context, name string, args map[string]any) (string, error) {
	switch name {
	case "memory_search":
		results, err := memory.Search(ctx, s.DB, strArg(args, "query"), intArg(args, "limit"))
		return memory.ResultsJSON(results), err
	case "memory_add_file":
		return "ok", memory.AddFile(ctx, s.DB, strArg(args, "path"), strArg(args, "context"))
	case "memory_add_directory":
		return "ok", memory.AddDirectory(ctx, s.DB, strArg(args, "path"), strArg(args, "context"))
	case "memory_add_text":
		return "ok", memory.AddText(ctx, s.DB, strArg(args, "text"), strArg(args, "context"))
	case "memory_clear":
		return "ok", memory.Clear(ctx, s.DB)
	case "memory_delete":
		return "ok", memory.Delete(ctx, s.DB, strArg(args, "hash"))
	case "memory_delete_context":
		return "ok", memory.DeleteContext(ctx, s.DB, strArg(args, "context"))
	case "memory_get":
		return memory.Get(ctx, s.DB, strArg(args, "hash"))
	case "memory_query":
		return memory.Query(ctx, s.DB, strArg(args, "query"))
	case "memory_reindex":
		return "ok", memory.Reindex(ctx, s.DB)
	case "memory_status":
		status, err := memory.Status(ctx, s.DB)
		data, _ := json.MarshalIndent(status, "", "  ")
		return string(data), err
	default:
		return "", fmt.Errorf("unknown tool %s", name)
	}
}

func ok(id any, result any) response {
	return response{JSONRPC: "2.0", ID: id, Result: result}
}

func fail(id any, code int, msg string) response {
	return response{JSONRPC: "2.0", ID: id, Error: errObj(code, msg)}
}

func errObj(code int, msg string) map[string]any {
	return map[string]any{"code": code, "message": msg}
}

func writeJSON(w http.ResponseWriter, v any) {
	w.Header().Set("Content-Type", "application/json")
	_ = json.NewEncoder(w).Encode(v)
}

func readFramedMessage(r *bufio.Reader) ([]byte, error) {
	length := -1
	for {
		line, err := r.ReadString('\n')
		if err != nil {
			return nil, err
		}
		line = strings.TrimRight(line, "\r\n")
		if line == "" {
			break
		}
		key, value, ok := strings.Cut(line, ":")
		if !ok || !strings.EqualFold(strings.TrimSpace(key), "Content-Length") {
			continue
		}
		n, err := strconv.Atoi(strings.TrimSpace(value))
		if err != nil || n < 0 {
			return nil, fmt.Errorf("invalid Content-Length")
		}
		length = n
	}
	if length < 0 {
		return nil, fmt.Errorf("missing Content-Length")
	}
	msg := make([]byte, length)
	_, err := io.ReadFull(r, msg)
	return msg, err
}

func writeFramedMessage(w io.Writer, v any) error {
	data, err := json.Marshal(v)
	if err != nil {
		return err
	}
	if _, err := fmt.Fprintf(w, "Content-Length: %d\r\n\r\n", len(data)); err != nil {
		return err
	}
	_, err = w.Write(data)
	return err
}

func strArg(args map[string]any, key string) string {
	if v, ok := args[key].(string); ok {
		return v
	}
	return ""
}

func intArg(args map[string]any, key string) int {
	switch v := args[key].(type) {
	case float64:
		return int(v)
	case int:
		return v
	case string:
		n, _ := strconv.Atoi(v)
		return n
	default:
		return 0
	}
}
