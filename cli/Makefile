BINARY := sqlmem

.PHONY: build test clean

build:
	go build -o $(BINARY) ./cmd/sqlmem

test:
	go test ./...

clean:
	rm -f $(BINARY)
