//! Just enough JSON for the console API.
//!
//! A writer and a top-level field scanner, not a parser. The agent builds two
//! flat objects and reads one key out of the reply; nothing here needs to hold
//! a document, so there is no `Value` type and no allocation per node.
//!
//! Upstream uses serde_json (`src/hbbs_http/sync.rs`). That is the obvious
//! choice anywhere else and is not available here: `serde_derive` is a proc
//! macro, and this crate's premise is a dependency list mrustc can build -- see
//! `Cargo.toml`. Two hundred lines is the cheaper side of that trade.
//!
//! The reader is deliberately structural. A heartbeat reply can carry a nested
//! `strategy` object, and a scanner that merely searched for `"sysinfo"` would
//! find one inside it; `skip_value` walks values rather than hunting for text,
//! so only a top-level key can ever match.

// ---------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------

/// A JSON object being built, in the order fields are added.
pub struct Object {
    buf: String,
    empty: bool,
}

impl Object {
    pub fn new() -> Self {
        Object { buf: String::from("{"), empty: true }
    }

    /// A string-valued field.
    ///
    /// `value` is escaped rather than interpolated: it is free-form text read
    /// off the machine -- a hostname, a login name -- and nothing guarantees a
    /// Mac's hostname contains no quote.
    pub fn string(mut self, key: &str, value: &str) -> Self {
        self.key(key);
        escape_into(value, &mut self.buf);
        self
    }

    /// An integer-valued field.
    pub fn number(mut self, key: &str, value: i64) -> Self {
        self.key(key);
        self.buf.push_str(&value.to_string());
        self
    }

    pub fn finish(mut self) -> String {
        self.buf.push('}');
        self.buf
    }

    fn key(&mut self, key: &str) {
        if !self.empty {
            self.buf.push(',');
        }
        self.empty = false;
        escape_into(key, &mut self.buf);
        self.buf.push(':');
    }
}

impl Default for Object {
    fn default() -> Self {
        Self::new()
    }
}

/// Append `s` to `out` as a quoted JSON string.
///
/// Escapes what RFC 8259 requires and nothing more: the two mandatory
/// characters and the C0 controls. Non-ASCII passes through as UTF-8, which is
/// valid JSON and what the server expects -- a Mac's hostname is routinely not
/// ASCII, and escaping it would be correct but pointlessly unreadable.
pub fn escape_into(s: &str, out: &mut String) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            '\u{8}' => out.push_str("\\b"),
            '\u{c}' => out.push_str("\\f"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
}

// ---------------------------------------------------------------------------
// Reading
// ---------------------------------------------------------------------------

/// The raw text of a top-level key's value, or `None` if it is not there.
///
/// Returns the value exactly as it appears, so `{"sysinfo":1}` yields `1` and
/// `{"strategy":{..}}` yields the whole object. Callers decide what it means;
/// see [`truthy`].
///
/// Malformed input yields `None` rather than panicking. This parses a reply
/// from a server the agent does not control, and the only safe reading of
/// nonsense is "the key is not there".
pub fn field<'a>(doc: &'a str, key: &str) -> Option<&'a str> {
    let b = doc.as_bytes();
    let mut i = skip_ws(b, 0);
    if *b.get(i)? != b'{' {
        return None;
    }
    i += 1;
    loop {
        i = skip_ws(b, i);
        if *b.get(i)? == b'}' {
            return None;
        }
        let (name, next) = string_at(b, i)?;
        i = skip_ws(b, next);
        if *b.get(i)? != b':' {
            return None;
        }
        i = skip_ws(b, i + 1);
        let start = i;
        i = skip_value(b, i)?;
        if name == key {
            return Some(&doc[start..i]);
        }
        i = skip_ws(b, i);
        if *b.get(i)? != b',' {
            return None;
        }
        i += 1;
    }
}

/// Whether a raw value means "yes".
///
/// The console answers `{"sysinfo":1}`, and a JSON `1` and `true` are the same
/// instruction. `0`, `false`, `null` and an absent key all mean no. Anything
/// else -- a non-empty string, an object -- is taken as yes: the only caller is
/// asking whether the server requested something, and a server that said
/// *something* did.
pub fn truthy(raw: &str) -> bool {
    match raw.trim() {
        "true" => true,
        "false" | "null" | "" => false,
        s => s.parse::<f64>().map(|n| n != 0.0).unwrap_or(true),
    }
}

fn skip_ws(b: &[u8], mut i: usize) -> usize {
    while i < b.len() && matches!(b[i], b' ' | b'\t' | b'\n' | b'\r') {
        i += 1;
    }
    i
}

/// The string starting at `i`, decoded, and the index just past its close quote.
///
/// `\uXXXX` is *not* decoded: it yields a replacement character, so an escaped
/// key simply fails to match. Nothing the agent reads is ever sent escaped, and
/// getting surrogate pairs right is more code than the case is worth.
fn string_at(b: &[u8], i: usize) -> Option<(String, usize)> {
    if *b.get(i)? != b'"' {
        return None;
    }
    // Bytes rather than chars, so a multi-byte sequence passes through whole
    // instead of being widened one byte at a time.
    let mut out: Vec<u8> = Vec::new();
    let mut j = i + 1;
    while j < b.len() {
        match b[j] {
            b'"' => return Some((String::from_utf8_lossy(&out).into_owned(), j + 1)),
            b'\\' => {
                match *b.get(j + 1)? {
                    b'"' => out.push(b'"'),
                    b'\\' => out.push(b'\\'),
                    b'/' => out.push(b'/'),
                    b'n' => out.push(b'\n'),
                    b'r' => out.push(b'\r'),
                    b't' => out.push(b'\t'),
                    b'b' => out.push(8),
                    b'f' => out.push(12),
                    b'u' => {
                        if j + 5 >= b.len() {
                            return None;
                        }
                        out.extend_from_slice("\u{fffd}".as_bytes());
                        j += 4;
                    }
                    _ => return None,
                }
                j += 2;
            }
            c => {
                out.push(c);
                j += 1;
            }
        }
    }
    None
}

/// The index just past the value starting at `i`.
fn skip_value(b: &[u8], i: usize) -> Option<usize> {
    match *b.get(i)? {
        b'"' => string_at(b, i).map(|(_, n)| n),
        b'{' => skip_nested(b, i, b'{', b'}'),
        b'[' => skip_nested(b, i, b'[', b']'),
        _ => {
            // A bare literal: a number, `true`, `false` or `null`. It ends at
            // the first structural character or space.
            let mut j = i;
            while j < b.len() && !matches!(b[j], b',' | b'}' | b']' | b' ' | b'\t' | b'\n' | b'\r') {
                j += 1;
            }
            if j == i {
                None
            } else {
                Some(j)
            }
        }
    }
}

/// The index just past a balanced `open`/`close` pair, ignoring both inside
/// strings -- a brace in a hostname must not change the depth.
fn skip_nested(b: &[u8], i: usize, open: u8, close: u8) -> Option<usize> {
    let mut depth = 0usize;
    let mut j = i;
    while j < b.len() {
        let c = b[j];
        if c == b'"' {
            j = string_at(b, j)?.1;
        } else if c == open {
            depth += 1;
            j += 1;
        } else if c == close {
            depth -= 1;
            j += 1;
            if depth == 0 {
                return Some(j);
            }
        } else {
            j += 1;
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_object_writes_its_fields_in_order() {
        let s = Object::new().string("id", "abc123").number("n", 7).finish();
        assert_eq!(s, "{\"id\":\"abc123\",\"n\":7}");
        assert_eq!(Object::new().finish(), "{}");
    }

    /// A hostname is not guaranteed to be JSON-safe, and this is the only thing
    /// standing between one and a malformed request the console silently drops.
    #[test]
    fn strings_are_escaped() {
        let s = Object::new().string("h", "a\"b\\c\nd\te").finish();
        assert_eq!(s, "{\"h\":\"a\\\"b\\\\c\\nd\\te\"}");
        let mut out = String::new();
        escape_into("\u{1}", &mut out);
        assert_eq!(out, "\"\\u0001\"");
    }

    /// UTF-8 survives: a Mac named in anything but ASCII must not arrive mangled.
    #[test]
    fn non_ascii_passes_through() {
        assert_eq!(Object::new().string("h", "Caf\u{e9}").finish(), "{\"h\":\"Caf\u{e9}\"}");
    }

    #[test]
    fn fields_are_found_by_name() {
        let doc = "{\"a\":1,\"sysinfo\":1,\"b\":\"x\"}";
        assert_eq!(field(doc, "sysinfo"), Some("1"));
        assert_eq!(field(doc, "b"), Some("\"x\""));
        assert_eq!(field(doc, "missing"), None);
        assert_eq!(field("{}", "sysinfo"), None);
    }

    /// The reason the reader is structural rather than a search: a heartbeat
    /// reply can carry a nested `strategy`, and a key inside it is not ours.
    #[test]
    fn nested_values_are_skipped_not_searched() {
        let doc = "{\"strategy\":{\"sysinfo\":1,\"deep\":[1,{\"sysinfo\":1}]},\"modified_at\":42}";
        assert_eq!(field(doc, "sysinfo"), None, "a nested key must not match");
        assert_eq!(field(doc, "modified_at"), Some("42"));
        assert!(field(doc, "strategy").unwrap().starts_with('{'));
    }

    /// A brace inside a string must not move the nesting depth.
    #[test]
    fn braces_inside_strings_do_not_count() {
        let doc = "{\"a\":{\"h\":\"}}}\"},\"b\":2}";
        assert_eq!(field(doc, "b"), Some("2"));
    }

    /// Every one of these is something a proxy or a wedged server can return,
    /// and none of them may take the agent down.
    #[test]
    fn malformed_input_yields_none_rather_than_panicking() {
        let bad = [
            "", "  ", "not json", "{", "{\"a\"", "{\"a\":", "{\"a\":}", "[1,2]",
            "{\"b\":1,", "{\"a\":\"unterminated", "{\"a\":{", "<html>502</html>",
        ];
        for doc in bad.iter() {
            assert_eq!(field(doc, "a"), None, "{:?} should not parse", doc);
        }
        // A document truncated *after* the key is still readable up to that
        // point, which is the correct reading rather than a lucky one.
        assert_eq!(field("{\"a\":1,", "a"), Some("1"));
    }

    #[test]
    fn truthiness_follows_the_json_value() {
        assert!(truthy("1"));
        assert!(truthy("true"));
        assert!(truthy("\"yes\""));
        assert!(!truthy("0"));
        assert!(!truthy("false"));
        assert!(!truthy("null"));
        assert!(!truthy(""));
    }

    /// Whitespace is legal everywhere between tokens, and a proxy may add it.
    #[test]
    fn whitespace_between_tokens_is_tolerated() {
        assert_eq!(field(" { \"a\" : 1 , \"b\" : 2 } ", "b"), Some("2"));
    }
}
