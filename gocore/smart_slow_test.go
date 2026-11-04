package main

import (
	"net"
	"sync/atomic"
	"testing"
	"time"
)

// readSlow reads a connection watched for a slowdown (window 400 ms, at
// least 1 KB, under 16 KB/s) until it ends or `limit` passes; `serve` is the
// server's side. It returns how many times onSlow was called, and the
// bytes the client got.
func readSlow(t *testing.T, limit time.Duration, serve func(server net.Conn, stop <-chan struct{})) (slow int32, got int) {
	t.Helper()
	oldWindow, oldMin := smartSlowWindow, smartSlowMin
	smartSlowWindow, smartSlowMin = 400*time.Millisecond, 1024
	t.Cleanup(func() { smartSlowWindow, smartSlowMin = oldWindow, oldMin })

	client, server := net.Pipe()
	stop := make(chan struct{})
	defer close(stop)
	go func() {
		defer server.Close()
		serve(server, stop)
	}()
	var slows, cuts atomic.Int32
	conn := watchSlow(client, 5*time.Second, func() { cuts.Add(1) }, func() { slows.Add(1) })
	defer conn.Close()
	_ = client.SetReadDeadline(time.Now().Add(limit))
	buffer := make([]byte, 512)
	for {
		n, err := conn.Read(buffer)
		got += n
		if err != nil {
			break
		}
	}
	if cuts.Load() != 0 {
		t.Fatal("a slowdown is not the cut")
	}
	return slows.Load(), got
}

// A record that comes in drops, 1 KB every 100 ms (10 KB/s).
func trickle(server net.Conn, stop <-chan struct{}) {
	_, _ = server.Write([]byte{0x17, 3, 3, 0x3e, 0x80}) // a 16000-byte record
	for i := 0; i < 15; i++ {
		select {
		case <-stop:
			return
		case <-time.After(100 * time.Millisecond):
		}
		if _, err := server.Write(make([]byte, 1024)); err != nil {
			return
		}
	}
}

func TestSlowTrickleIsCaught(t *testing.T) {
	slow, got := readSlow(t, 4*time.Second, trickle)
	if slow != 1 {
		t.Fatalf("a record held to 10 KB/s: onSlow called %d times", slow)
	}
	if got == 0 || got >= 15*1024 {
		t.Fatalf("the connection should end early, with what came delivered: got %d", got)
	}
}

func TestFastRecordsAreNotSlow(t *testing.T) {
	slow, got := readSlow(t, 3*time.Second, func(server net.Conn, stop <-chan struct{}) {
		for sent := 0; sent < 40000; sent += 16005 {
			_, _ = server.Write(record(16000))
		}
	})
	if slow != 0 || got < 40000 {
		t.Fatalf("a fast stream: slow %d, got %d", slow, got)
	}
}

func TestIdleBetweenRecordsIsNotSlow(t *testing.T) {
	slow, _ := readSlow(t, 1500*time.Millisecond, func(server net.Conn, stop <-chan struct{}) {
		_, _ = server.Write(record(2000)) // a whole record, then keep-alive silence
		<-stop
	})
	if slow != 0 {
		t.Fatal("a connection idle between records waits for nothing")
	}
}

func TestNoSlowWithoutOnSlow(t *testing.T) {
	oldWindow, oldMin := smartSlowWindow, smartSlowMin
	smartSlowWindow, smartSlowMin = 400*time.Millisecond, 1024
	defer func() { smartSlowWindow, smartSlowMin = oldWindow, oldMin }()
	client, server := net.Pipe()
	stop := make(chan struct{})
	defer close(stop)
	go func() {
		defer server.Close()
		trickle(server, stop)
	}()
	conn := watchCut(client, 5*time.Second, func() {})
	defer conn.Close()
	_ = client.SetReadDeadline(time.Now().Add(2500 * time.Millisecond))
	buffer := make([]byte, 512)
	got := 0
	for {
		n, err := conn.Read(buffer)
		got += n
		if err != nil {
			break
		}
	}
	if got < 15*1024 {
		t.Fatalf("watchCut alone leaves a slow stream be: got %d", got)
	}
}
