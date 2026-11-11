/* Crash-report attachments: the account, profile-folder and machine names are
 * removed in every encoding a log can carry them in, log tails stay whole
 * lines, and the logs are trimmed (oldest text first) to fit the payload cap. */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../src/ui/webview/report_scrub.h"

static int contains(const char *hay, size_t hl, const void *needle, size_t nl)
{
    size_t i;
    for (i = 0; nl && i + nl <= hl; i++)
        if (memcmp(hay + i, needle, nl) == 0) return 1;
    return 0;
}

static size_t scrub(const rs_forms *f, const char *src, size_t len, char *out, size_t cap)
{
    size_t n = rs_scrub_forms(out, cap, src, len, f);
    if (n < cap) out[n] = '\0';
    return n;
}

int main(void)
{
    char out[1024];
    size_t n;

    /* An ASCII account name, in any case, inside a profile path and as a word. */
    {
        rs_forms f; memset(&f, 0, sizeof f);
        rs_add_name(&f, L"SampleUser", "<user>");
        const char *src = "log: C:\\Users\\SampleUser\\thing.log opened by sampleuser";
        scrub(&f, src, strlen(src), out, sizeof out);
        assert(strcmp(out, "log: C:\\Users\\<user>\\thing.log opened by <user>") == 0);
    }

    /* A non-ASCII name: the UTF-8 log line, the UTF-16LE dump and both cases go. */
    {
        rs_forms f; memset(&f, 0, sizeof f);
        const wchar_t *name = L"J\x00f6rg";                /* Joerg with o-umlaut */
        rs_add_name(&f, name, "<user>");
        const char utf8[] = "user=J\xc3\xb6rg path=C:\\Users\\J\xc3\x96RG\\x";   /* as written and upper-cased */
        n = scrub(&f, utf8, sizeof utf8 - 1, out, sizeof out);
        assert(strcmp(out, "user=<user> path=C:\\Users\\<user>\\x") == 0);

        const wchar_t wide[] = L"[J\x00f6rg]";
        n = scrub(&f, (const char *)wide, sizeof wide - sizeof(wchar_t), out, sizeof out);
        assert(!contains(out, n, name, 4 * sizeof(wchar_t)));
        assert(contains(out, n, L"<user>", 6 * sizeof(wchar_t)));
        assert(n == (1 + 6 + 1) * sizeof(wchar_t));
    }

    /* The ANSI code page form, when the name is representable in it. */
    {
        rs_forms f; memset(&f, 0, sizeof f);
        const wchar_t *name = L"Ren\x00e9";                /* Rene with e-acute */
        char ansi[16]; BOOL lossy = FALSE;
        int al = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, name, 4, ansi, (int)sizeof ansi, NULL, &lossy);
        rs_add_name(&f, name, "<user>");
        if (al > 0 && !lossy) {
            char src[64];
            size_t sl = 0;
            memcpy(src, "by ", 3); sl = 3;
            memcpy(src + sl, ansi, (size_t)al); sl += (size_t)al;
            n = scrub(&f, src, sl, out, sizeof out);
            assert(strcmp(out, "by <user>") == 0);
        }
    }

    /* The profile folder's own name, which can differ from the account name. */
    {
        rs_forms f; memset(&f, 0, sizeof f);
        rs_add_name(&f, L"Sam Example", "<user>");
        rs_add_path_leaf(&f, L"C:\\Users\\sexam", "<user>");
        const char *src = "C:\\Users\\sexam\\AppData by Sam Example";
        scrub(&f, src, strlen(src), out, sizeof out);
        assert(strcmp(out, "C:\\Users\\<user>\\AppData by <user>") == 0);
    }

    /* Machine name mid-token; a one-character name is never scrubbed. */
    {
        rs_forms f; memset(&f, 0, sizeof f);
        rs_add_name(&f, L"DESKTOP-AB12", "<machine>");
        rs_add_name(&f, L"a", "<user>");
        const char *src = "\\\\DESKTOP-AB12\\share on DESKTOP-ab12, banana";
        scrub(&f, src, strlen(src), out, sizeof out);
        assert(strcmp(out, "\\\\<machine>\\share on <machine>, banana") == 0);
    }

    /* Output stops at the capacity. */
    {
        rs_forms f; memset(&f, 0, sizeof f);
        char tiny[8];
        rs_add_name(&f, L"zz", "<user>");
        n = rs_scrub_forms(tiny, sizeof tiny, "aaaa bbbb cccc", 14, &f);
        assert(n == sizeof tiny);
    }

    /* tail: short buffer -> from 0; prefer the next whole line; one giant line -> byte cut */
    assert(rs_tail_offset("abc\ndef\n", 8, 100) == 0);
    {
        const char *b = "line1\nline2\nline3\n";
        size_t off = rs_tail_offset(b, 18, 10);
        assert(off == 12 && strncmp(b + off, "line3", 5) == 0);
    }
    assert(rs_tail_offset("aaaaaaaaaaaaaaaaaaaa", 20, 5) == 15);

    /* Fitting the logs: everything that fits is kept; otherwise the newest whole lines,
     * counted as escaped JSON, and a cut never splits a UTF-8 sequence. */
    {
        const char *b = "old line\nmid \"q\"\nnew line\n";
        size_t len = strlen(b), off, used = 0, i;
        assert(rs_fit_escaped_tail(b, len, 1000) == 0);
        off = rs_fit_escaped_tail(b, len, 20);
        assert(strcmp(b + off, "new line\n") == 0);
        for (i = off; i < len; i++) used += rs_json_escaped_len((unsigned char)b[i]);
        assert(used <= 20);
        assert(rs_fit_escaped_tail(b, len, 0) == len);
    }
    {
        const char b[] = "\xc3\xb6\xc3\xb6\xc3\xb6";   /* three two-byte characters, no newline */
        size_t off = rs_fit_escaped_tail(b, 6, 3);
        assert(off == 4);
    }

    printf("report_scrub_test OK\n");
    return 0;
}
