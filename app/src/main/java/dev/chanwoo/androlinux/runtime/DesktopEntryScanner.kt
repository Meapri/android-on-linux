/*
 * DesktopEntryScanner — discovers installed Linux GUI apps by scanning the rootfs
 * `usr/share/applications` directory for `.desktop` files and mapping each to InstalledApp.
 *
 * This is the Kotlin mirror of the host-verified tools/desktop_entry.py (Desktop Entry
 * Specification 1.5, launcher subset). The parsing rules are intentionally identical so
 * on-device behavior matches the pytest-verified reference:
 *   - INI groups; only [Desktop Entry] is interpreted; tolerant (skip blank/#/no-'='
 *     lines); later duplicate keys win.
 *   - Localized keys (Name[ko], …) resolved against the device locale with the spec
 *     fallback chain (lang_COUNTRY@MOD → lang_COUNTRY → lang@MOD → lang), else base key.
 *   - Exec field codes (%f %F %u %U %d %D %n %N %v %m removed; %% → %; %i → --icon <icon>;
 *     %c → Name; %k → .desktop path) expanded/stripped when building argv.
 *   - Visibility filter: Type=Application only, drop NoDisplay=true / Hidden=true.
 *   - Terminal=true is ALSO dropped for Phase 1 (no TTY host yet).
 *   - Icon name → hicolor/pixmaps path resolution (deterministic size/ext search).
 *
 * Mapping → InstalledApp:
 *   appId   = .desktop basename without ".desktop" (e.g. "gimp"); stable launch key.
 *   name    = localized Name.
 *   entry   = LaunchEntry.EXEC(target = first argv token resolved to an ABSOLUTE rootfs
 *             binary path, args = remaining argv tokens). EXEC (not DESKTOP) because the
 *             Exec line is already field-code-expanded here, so the loader gets a clean
 *             argv — exactly the shape runChromiumStandalone feeds (binary + args).
 *   category = freedesktop Categories → AppCategory (best-effort; default UTILITY).
 *   iconPath = resolved hicolor/pixmaps path under the rootfs, or null.
 *
 * Pure + host-testable: parsing/expansion/mapping helpers take strings and a path→exists
 * predicate, so they run with no Android and no filesystem (see tests). scan(File) is the
 * only filesystem-touching entry point; a missing dir yields an empty list (no throw).
 */
package dev.chanwoo.androlinux.runtime

import java.io.File

object DesktopEntryScanner {

    private const val DESKTOP_ENTRY_GROUP = "Desktop Entry"

    /** Field codes that take no launcher argument and expand to nothing (no files/URLs). */
    private val VALUELESS_FIELD_CODES = "fFuUdDnNvm".toSet()

    /** hicolor sizes a launcher grid wants, largest first (upsampling beats inventing detail). */
    private val HICOLOR_SIZES = listOf(
        "512x512", "256x256", "192x192", "128x128", "96x96",
        "64x64", "48x48", "32x32", "24x24", "16x16",
    )
    private val ICON_EXTENSIONS = listOf("png", "svg", "xpm")

    private val BOOL_TRUE = setOf("true", "1", "yes")

    /** Common rootfs bin dirs a bare Exec command is resolved against (PATH-like order). */
    private val BIN_DIRS = listOf("usr/bin", "bin", "usr/local/bin", "usr/sbin", "sbin", "usr/games")

    // ----------------------------------------------------------------------- //
    // Public entry point
    // ----------------------------------------------------------------------- //

    /**
     * Scan the rootfs `usr/share/applications` dir for `.desktop` files → launcher apps.
     * Returns visible (non-NoDisplay/Hidden), non-Terminal Application entries, sorted
     * case-insensitively by display name. Missing/unreadable dir → empty list.
     *
     * @param locale device locale tag (e.g. "ko_KR") for localized Name resolution.
     */
    fun scan(rootfsDir: File, locale: String? = defaultLocale()): List<InstalledApp> {
        val appsDir = File(File(File(rootfsDir, "usr"), "share"), "applications")
        val files = appsDir.listFiles { f -> f.isFile && f.name.endsWith(".desktop") }
            ?: return emptyList()
        // path→exists predicate: a rootfs-absolute path "/usr/bin/x" maps to the file
        // under rootfsDir. Used for Exec target resolution + icon existence checks.
        val exists: (String) -> Boolean = { abs -> File(rootfsDir, abs.removePrefix("/")).exists() }

        val byId = LinkedHashMap<String, InstalledApp>()
        for (f in files) {
            val text = runCatching { f.readText() }.getOrNull() ?: continue
            val app = parseToInstalledApp(text, desktopPath = f.absolutePath, locale = locale, exists = exists)
                ?: continue
            byId[app.appId] = app  // later same-id wins (matches XDG_DATA_DIRS precedence)
        }
        return byId.values.sortedBy { it.name.lowercase() }
    }

    private fun defaultLocale(): String? =
        runCatching { java.util.Locale.getDefault().toString().takeIf { it.isNotBlank() } }.getOrNull()

    // ----------------------------------------------------------------------- //
    // Parse one .desktop → InstalledApp (pure; exists predicate injected)
    // ----------------------------------------------------------------------- //

    /**
     * Parse a `.desktop` body into an InstalledApp, or null if it is not a launchable,
     * visible, non-terminal Application. [exists] tests whether a rootfs-absolute path is
     * present (for Exec target + icon resolution); pass `{ true }` in pure tests.
     */
    fun parseToInstalledApp(
        text: String,
        desktopPath: String,
        locale: String? = null,
        exists: (String) -> Boolean = { true },
    ): InstalledApp? {
        val group = parseGroups(text)[DESKTOP_ENTRY_GROUP] ?: return null
        if (group["Type"]?.trim() != "Application") return null

        val name = localizedValue(group, "Name", locale)
        val execRaw = group["Exec"]
        if (name.isBlank() || execRaw.isNullOrBlank()) return null

        // Phase-1 visibility: drop hidden + terminal apps (no TTY host yet).
        if (toBool(group["NoDisplay"]) || toBool(group["Hidden"])) return null
        if (toBool(group["Terminal"])) return null

        val iconName = group["Icon"].orEmpty()
        val appId = appIdFromPath(desktopPath)
        val argv = expandExec(execRaw, icon = iconName, desktopPath = desktopPath, name = name)
        if (argv.isEmpty()) return null

        // Resolve the binary token to an absolute rootfs path. Prefer TryExec (a bare
        // command name per spec, the canonical "is this installed" probe) if present,
        // else the first Exec argv token. Either way the args are Exec's argv[1..] —
        // Exec's argv[0] is the program, which TryExec just names more reliably.
        val tryExec = group["TryExec"]?.trim().orEmpty()
        val binToken = tryExec.ifBlank { argv.first() }
        val target = resolveBinary(binToken, exists)
        val args = argv.drop(1)

        val iconPath = resolveIconPath(iconName, exists)
        val category = mapCategory(splitList(group["Categories"].orEmpty()))

        return InstalledApp(
            appId = appId,
            name = name,
            summary = localizedValue(group, "Comment", locale).ifBlank {
                localizedValue(group, "GenericName", locale)
            },
            entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, target, args),
            category = category,
            iconPath = iconPath,
        )
    }

    // ----------------------------------------------------------------------- //
    // Low-level INI parsing (mirror of desktop_entry.parse_groups)
    // ----------------------------------------------------------------------- //

    /** Parse a `.desktop` body into {group → {key → value}}. Tolerant; later keys win. */
    fun parseGroups(text: String): Map<String, Map<String, String>> {
        val groups = LinkedHashMap<String, LinkedHashMap<String, String>>()
        var current: LinkedHashMap<String, String>? = null
        for (raw in text.split("\n")) {
            val line = raw.trim()
            if (line.isEmpty() || line.startsWith("#")) continue
            if (line.startsWith("[") && line.endsWith("]")) {
                val gname = line.substring(1, line.length - 1).trim()
                current = groups.getOrPut(gname) { LinkedHashMap() }
                continue
            }
            if (current == null) continue  // keys before any group header: invalid, skip
            val eq = line.indexOf('=')
            if (eq < 0) continue
            current[line.substring(0, eq).trim()] = line.substring(eq + 1).trim()
        }
        return groups
    }

    // ----------------------------------------------------------------------- //
    // Localized key resolution (mirror of desktop_entry._locale_candidates)
    // ----------------------------------------------------------------------- //

    private fun localeCandidates(locale: String?): List<String> {
        if (locale.isNullOrBlank()) return emptyList()
        var base = locale
        var modifier = ""
        val at = base.indexOf('@')
        if (at >= 0) { modifier = base.substring(at + 1); base = base.substring(0, at) }
        val dot = base.indexOf('.')
        if (dot >= 0) base = base.substring(0, dot)
        var lang = base
        var country = ""
        val us = base.indexOf('_')
        if (us >= 0) { lang = base.substring(0, us); country = base.substring(us + 1) }

        val out = ArrayList<String>()
        if (country.isNotEmpty() && modifier.isNotEmpty()) out.add("${lang}_$country@$modifier")
        if (country.isNotEmpty()) out.add("${lang}_$country")
        if (modifier.isNotEmpty()) out.add("$lang@$modifier")
        out.add(lang)
        return out.distinct()
    }

    /** Return key[locale] with spec fallback, else the unlocalized key, else "". */
    fun localizedValue(group: Map<String, String>, key: String, locale: String?): String {
        for (cand in localeCandidates(locale)) {
            group["$key[$cand]"]?.let { return it }
        }
        return group[key].orEmpty()
    }

    // ----------------------------------------------------------------------- //
    // Exec field-code expansion (mirror of desktop_entry.expand_exec)
    // ----------------------------------------------------------------------- //

    /** Expand an Exec= value into a runnable argv list (field codes stripped/expanded). */
    fun expandExec(execValue: String, icon: String = "", desktopPath: String = "", name: String = ""): List<String> {
        if (execValue.isBlank()) return emptyList()
        val tokens = shlexSplit(execValue)
        val argv = ArrayList<String>()
        for (token in tokens) argv.addAll(expandToken(token, icon, desktopPath, name))
        return argv
    }

    private fun expandToken(token: String, icon: String, desktopPath: String, name: String): List<String> {
        // %i alone → two args ("--icon", value) — the spec's only sanctioned %i use.
        if (token == "%i") return if (icon.isNotEmpty()) listOf("--icon", icon) else emptyList()

        val outChars = StringBuilder()
        val extra = ArrayList<String>()
        var i = 0
        while (i < token.length) {
            val ch = token[i]
            if (ch != '%') { outChars.append(ch); i++; continue }
            if (i + 1 >= token.length) { i++; continue }  // trailing lone '%' → drop
            val code = token[i + 1]
            i += 2
            when {
                code == '%' -> outChars.append('%')
                code in VALUELESS_FIELD_CODES -> { /* removed */ }
                code == 'c' -> outChars.append(name)
                code == 'k' -> outChars.append(desktopPath)
                code == 'i' -> if (icon.isNotEmpty()) extra.add(icon)
                else -> { /* unknown/deprecated field code → strip */ }
            }
        }
        val result = ArrayList<String>()
        val text = outChars.toString()
        if (text.isNotEmpty()) result.add(text)
        result.addAll(extra)
        return result
    }

    /**
     * Minimal POSIX-shell word split honoring '…' and "…" quoting + backslash escapes —
     * enough for Exec= lines (mirrors Python shlex.split for this subset). Unbalanced
     * quotes fall back to a naive whitespace split (matching the Python except path).
     */
    fun shlexSplit(s: String): List<String> {
        val out = ArrayList<String>()
        val cur = StringBuilder()
        var inSingle = false
        var inDouble = false
        var hasToken = false
        var i = 0
        while (i < s.length) {
            val c = s[i]
            when {
                inSingle -> {
                    if (c == '\'') inSingle = false else cur.append(c)
                    hasToken = true
                }
                inDouble -> {
                    if (c == '\\' && i + 1 < s.length && (s[i + 1] == '"' || s[i + 1] == '\\' || s[i + 1] == '$' || s[i + 1] == '`')) {
                        cur.append(s[i + 1]); i++
                    } else if (c == '"') {
                        inDouble = false
                    } else cur.append(c)
                    hasToken = true
                }
                c == '\'' -> { inSingle = true; hasToken = true }
                c == '"' -> { inDouble = true; hasToken = true }
                c == '\\' && i + 1 < s.length -> { cur.append(s[i + 1]); i++; hasToken = true }
                c.isWhitespace() -> {
                    if (hasToken) { out.add(cur.toString()); cur.clear(); hasToken = false }
                }
                else -> { cur.append(c); hasToken = true }
            }
            i++
        }
        if (inSingle || inDouble) return s.trim().split(Regex("\\s+")).filter { it.isNotEmpty() }
        if (hasToken) out.add(cur.toString())
        return out
    }

    // ----------------------------------------------------------------------- //
    // Binary + icon path resolution
    // ----------------------------------------------------------------------- //

    /**
     * Resolve a command token to an absolute rootfs binary path. Absolute tokens are
     * returned as-is. A bare name is searched across BIN_DIRS (first existing wins);
     * if none exists, fall back to "/usr/bin/<name>" so launch still has a concrete
     * path (the loader reports a clean "no such file" if truly absent).
     */
    fun resolveBinary(token: String, exists: (String) -> Boolean): String {
        if (token.isBlank()) return token
        if (token.startsWith("/")) return token
        val base = token.substringAfterLast('/')  // defensive: strip any relative dir
        for (dir in BIN_DIRS) {
            val cand = "/$dir/$base"
            if (exists(cand)) return cand
        }
        return "/usr/bin/$base"
    }

    /**
     * Resolve an Icon= value to a concrete rootfs path (Icon Theme Spec subset):
     *   absolute → as-is; else hicolor/<size>/apps/<icon>.<ext> (largest first),
     *   then hicolor/scalable/apps/<icon>.svg, then pixmaps/<icon>.<ext>.
     * Returns the first path that [exists], else null.
     */
    fun resolveIconPath(
        icon: String,
        exists: (String) -> Boolean,
        themeRoot: String = "/usr/share/icons/hicolor",
        pixmapsDir: String = "/usr/share/pixmaps",
    ): String? {
        if (icon.isBlank()) return null
        if (icon.startsWith("/")) return if (exists(icon)) icon else icon  // trust absolute icon
        val candidates = ArrayList<String>()
        for (size in HICOLOR_SIZES) for (ext in ICON_EXTENSIONS) candidates.add("$themeRoot/$size/apps/$icon.$ext")
        candidates.add("$themeRoot/scalable/apps/$icon.svg")
        for (ext in ICON_EXTENSIONS) candidates.add("$pixmapsDir/$icon.$ext")
        return candidates.firstOrNull { exists(it) }
    }

    // ----------------------------------------------------------------------- //
    // Category mapping: freedesktop Categories → ALR AppCategory
    // ----------------------------------------------------------------------- //

    /**
     * Map a .desktop Categories list to an AppCategory. Walks the list, returning the
     * first token with a known mapping (registered Main Categories take priority via
     * the lookup table order). Default UTILITY when nothing matches.
     */
    fun mapCategory(categories: List<String>): AppCategory {
        for (cat in categories) FREEDESKTOP_TO_ALR[cat]?.let { return it }
        return AppCategory.UTILITY
    }

    // freedesktop category token → ALR AppCategory. Covers the registered Main
    // Categories plus a few common additional categories; everything else → UTILITY.
    private val FREEDESKTOP_TO_ALR: Map<String, AppCategory> = mapOf(
        "Graphics" to AppCategory.GRAPHICS,
        "2DGraphics" to AppCategory.GRAPHICS,
        "RasterGraphics" to AppCategory.GRAPHICS,
        "VectorGraphics" to AppCategory.GRAPHICS,
        "Photography" to AppCategory.GRAPHICS,
        "Development" to AppCategory.DEVELOPMENT,
        "IDE" to AppCategory.DEVELOPMENT,
        "Building" to AppCategory.DEVELOPMENT,
        "Debugger" to AppCategory.DEVELOPMENT,
        "Office" to AppCategory.OFFICE,
        "WordProcessor" to AppCategory.OFFICE,
        "Spreadsheet" to AppCategory.OFFICE,
        "Presentation" to AppCategory.OFFICE,
        "Network" to AppCategory.INTERNET,
        "WebBrowser" to AppCategory.INTERNET,
        "Email" to AppCategory.INTERNET,
        "InstantMessaging" to AppCategory.INTERNET,
        "AudioVideo" to AppCategory.MULTIMEDIA,
        "Audio" to AppCategory.MULTIMEDIA,
        "Video" to AppCategory.MULTIMEDIA,
        "Player" to AppCategory.MULTIMEDIA,
        "Game" to AppCategory.GAMES,
        "System" to AppCategory.SYSTEM,
        "Monitor" to AppCategory.SYSTEM,
        "Security" to AppCategory.SYSTEM,
        "Settings" to AppCategory.SYSTEM,
        "Utility" to AppCategory.UTILITY,
        "Accessibility" to AppCategory.UTILITY,
        "Archiving" to AppCategory.UTILITY,
        "FileTools" to AppCategory.UTILITY,
        "Education" to AppCategory.EDUCATION,
        "Science" to AppCategory.EDUCATION,
        "TerminalEmulator" to AppCategory.TERMINAL,
    )

    // ----------------------------------------------------------------------- //
    // Small helpers
    // ----------------------------------------------------------------------- //

    /** `/usr/share/applications/org.gimp.GIMP.desktop` → `org.gimp.GIMP`. */
    fun appIdFromPath(path: String): String {
        val base = path.substringAfterLast('/')
        return if (base.endsWith(".desktop")) base.dropLast(".desktop".length) else base
    }

    /** Split a ';'-separated spec list, dropping empties (incl. the trailing one). */
    fun splitList(value: String): List<String> =
        if (value.isEmpty()) emptyList() else value.split(";").filter { it.isNotEmpty() }

    private fun toBool(value: String?): Boolean = value?.trim()?.lowercase() in BOOL_TRUE
}
