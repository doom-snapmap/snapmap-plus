package main

import (
	"fmt"
	"strings"
	"sync"
	"time"
	"unicode"
)

// A slow archive or mod can take several seconds even when the current item doesn't change.
type migrationProgress struct {
	mu            sync.Mutex
	phase, item   string
	since         time.Time
	done, stopped chan struct{}
}

func startMigrationProgress() *migrationProgress {
	p := &migrationProgress{done: make(chan struct{}), stopped: make(chan struct{})}
	go func() {
		defer close(p.stopped)
		ticker := time.NewTicker(4 * time.Second)
		defer ticker.Stop()
		for {
			select {
			case now := <-ticker.C:
				p.report(now)
			case <-p.done:
				return
			}
		}
	}()
	return p
}

func (p *migrationProgress) stop() {
	close(p.done)
	<-p.stopped
	ui.clearActivity()
}

func (p *migrationProgress) step(phase string) {
	if p == nil {
		return
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	p.phase, p.item, p.since = phase, "", time.Now()
	ui.Note("%s", phase)
}

func (p *migrationProgress) detail(item string) {
	if p == nil {
		return
	}
	p.mu.Lock()
	p.item = item
	p.mu.Unlock()
}

func (p *migrationProgress) position(i, total int) {
	if p != nil {
		p.detail(fmt.Sprintf("%d of %d", i, total))
	}
}

func (p *migrationProgress) report(now time.Time) {
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.phase == "" || now.Sub(p.since) < 3*time.Second {
		return
	}
	text := p.phase
	if p.item != "" {
		text += "  " + briefItem(p.item)
	}
	ui.activity(fmt.Sprintf("%s  (%s)", text, now.Sub(p.since).Truncate(time.Second)))
}

func briefItem(name string) string {
	item := []rune(strings.Map(func(r rune) rune {
		if unicode.IsControl(r) {
			return ' '
		}
		return r
	}, name))
	if len(item) > 36 {
		item = append(item[:33], '.', '.', '.')
	}
	return string(item)
}
