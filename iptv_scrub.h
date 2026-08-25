#ifndef IPTV_SCRUB_H
#define IPTV_SCRUB_H

/*
 * Taking the viewer out of the log.
 *
 * The log is the thing people send to somebody else. Every message in this core
 * that says "check the [VLC-LOG] lines" is an instruction to copy a file and
 * paste it where a stranger can read it, and that is exactly the right advice -
 * it is how the difficult bugs in this core were found. It is also why what
 * goes into that file matters.
 *
 * Two kinds of thing must not be in there.
 *
 * The first is the viewer. A URL that yt-dlp resolves carries the address it
 * was resolved for, in plain text and in the middle of the path:
 *
 *   .../hls_playlist/expire/1787645434/ei/mvmMapau.../ip/198.51.100.7/id/...
 *
 * Nobody put it there on purpose and nobody notices it while reading, because
 * it is one field among thirty in a line nine hundred characters long. It is
 * still the viewer's home address, written down every time they change channel.
 *
 * The second is anything that would let the reader of the log take the stream.
 * The signature on a resolved URL is a bearer token with hours left on it, and
 * an IPTV list in the usual Xtream shape puts the subscriber's user name and
 * password in the path of every single channel:
 *
 *   http://host:8080/joao/segredo123/45678.ts
 *
 * That one is worse than the IP, because it does not expire.
 *
 * So this runs on every line, from one place, before anything is written. Not
 * at the call sites: most of the lines are libVLC's, and libVLC announces the
 * full URL of everything it opens. Code that is not ours cannot be asked to
 * remember, so it is not asked.
 *
 * There is no switch to turn it off. A diagnostic that is only private when
 * somebody remembered to enable it is not private, and nothing removed here has
 * ever been what a diagnosis turned on: the host, the protocol, the expiry and
 * the error are all still there.
 */

/*
 * Removes those fields from `text`, in place.
 *
 * The text only ever gets shorter - a value is deleted, never replaced with
 * something longer - so this cannot overflow whatever buffer it was handed, and
 * it needs no second buffer on a path that runs hundreds of thousands of times
 * a session.
 *
 * Idempotent: running it twice does nothing the second time, which is what lets
 * a line pass through both note() and iptv_log() without either of them having
 * to know whether the other already did it.
 */
void iptv_scrub(char *text);

#endif
