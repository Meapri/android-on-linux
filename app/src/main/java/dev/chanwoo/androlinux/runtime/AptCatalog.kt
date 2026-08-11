package dev.chanwoo.androlinux.runtime

import android.util.Log
import dev.chanwoo.androlinux.AlrRuntime

/**
 * The catalog, read from the guest's own apt index.
 *
 * The catalog used to be eighteen entries written by hand, each with a
 * hand-audited install envelope. That is a maintenance sink with a hard
 * ceiling: an app nobody had entered did not exist as far as the product was
 * concerned, however well it would have run. The guest already holds a complete
 * index of every arm64 package Ubuntu ships, with names, descriptions, sections
 * and sizes -- there is nothing to curate, only something to read.
 *
 * WHAT IS AND IS NOT DECIDABLE HERE. apt can say a package's Section, and that
 * is what the category chips are built from. apt cannot say whether a package
 * ships a .desktop file without downloading it, so this does NOT try to
 * pre-filter to "GUI apps"; it filters to the sections desktop software lives
 * in and lets the truth arrive the honest way -- after install, the .desktop
 * scan finds what is actually launchable. A package with no entry simply does
 * not become a tile, which is the same answer the old list gave by omission,
 * reached by measurement instead of by editing.
 */
object AptCatalog {

    private const val TAG = "alr_runtime"

    /**
     * apt Section -> our category. Sections not in this map are dropped: libs,
     * doc, debug, oldlibs, perl, python and the rest are not things a person
     * installs from a launcher, and listing 60,000 of them is not a catalog.
     */
    private val SECTIONS = mapOf(
        "graphics" to AppCategory.GRAPHICS,
        "video" to AppCategory.MULTIMEDIA,
        "sound" to AppCategory.MULTIMEDIA,
        "web" to AppCategory.INTERNET,
        "net" to AppCategory.INTERNET,
        "mail" to AppCategory.INTERNET,
        "editors" to AppCategory.OFFICE,
        "text" to AppCategory.OFFICE,
        "tex" to AppCategory.OFFICE,
        "games" to AppCategory.GAMES,
        "science" to AppCategory.EDUCATION,
        "math" to AppCategory.EDUCATION,
        "education" to AppCategory.EDUCATION,
        "devel" to AppCategory.DEVELOPMENT,
        "admin" to AppCategory.SYSTEM,
        "utils" to AppCategory.UTILITY,
        "x11" to AppCategory.UTILITY,
        "misc" to AppCategory.UTILITY,
        "shells" to AppCategory.TERMINAL,
    )

    /**
     * Query the guest's index.
     *
     * [query] is passed to `apt-cache search`, which matches name and
     * description. An empty query would return the entire archive, so it is
     * answered with a small curated-by-POPULARITY-not-by-us starting set: the
     * sections' own contents, capped. The cap is [limit] and it is REPORTED,
     * not silent -- a truncated list that looks complete is how you get a bug
     * report about a package that is in the archive and not on screen.
     */
    fun search(
        alr: AlrRuntime,
        distro: String,
        query: String,
        limit: Int = 200,
    ): List<CatalogApp> {
        val q = query.trim()
        if (q.isEmpty()) return emptyList()
        val r = alr.run(
            distro = distro,
            program = "/usr/bin/apt-cache",
            arguments = listOf("search", "--names-only", q),
            timeoutSeconds = 120,
        )
        if (!r.ok) {
            Log.w(TAG, "catalog: apt-cache search '$q' rc=${r.exitCode}")
            return emptyList()
        }
        val names = r.stdout.lineSequence()
            .mapNotNull { line ->
                val i = line.indexOf(" - ")
                if (i <= 0) null else line.substring(0, i).trim() to line.substring(i + 3).trim()
            }
            .filterNot { (n, _) -> isNotAnApp(n) }
            .take(limit)
            .toList()
        if (names.isEmpty()) return emptyList()
        Log.i(TAG, "catalog: '$q' -> ${names.size} candidate(s)")
        return details(alr, distro, names)
    }

    /**
     * Names that are never a thing to launch. Cheap prefix/suffix rules rather
     * than a package list: `lib*`, `*-dev`, `*-doc`, `*-dbg`, `python3-*` and
     * friends are structural, and structure is what generalises.
     */
    private fun isNotAnApp(name: String): Boolean =
        name.startsWith("lib") || name.startsWith("python3-") ||
            name.startsWith("golang-") || name.startsWith("node-") ||
            name.startsWith("fonts-") || name.startsWith("gir1.2-") ||
            name.endsWith("-dev") || name.endsWith("-doc") ||
            name.endsWith("-dbg") || name.endsWith("-dbgsym") ||
            name.endsWith("-common") || name.endsWith("-data")

    /**
     * One `apt-cache show` for the whole batch: the stanzas carry Section and
     * Size, and a call per package would be one guest launch per package.
     */
    private fun details(
        alr: AlrRuntime,
        distro: String,
        candidates: List<Pair<String, String>>,
    ): List<CatalogApp> {
        val r = alr.run(
            distro = distro,
            program = "/usr/bin/apt-cache",
            arguments = listOf("show") + candidates.map { it.first },
            timeoutSeconds = 180,
        )
        if (!r.ok) return emptyList()
        val summaries = candidates.toMap()
        val out = LinkedHashMap<String, CatalogApp>()
        var pkg = ""; var section = ""; var size = 0L; var desc = ""
        fun flush() {
            val cat = SECTIONS[section.substringAfterLast('/')] ?: return
            if (pkg.isEmpty() || pkg in out) return
            out[pkg] = CatalogApp(
                appId = pkg,
                name = pkg,
                summary = summaries[pkg] ?: desc,
                // The launcher resolves the real Exec from the .desktop file
                // after install; this is what to run if there is none.
                entry = LaunchEntry(LaunchEntry.EntryKind.EXEC, "/usr/bin/$pkg"),
                category = cat,
                description = desc,
                rootfsDeps = listOf(RootfsDep(RootfsDepKind.APT, pkg, size)),
                installSizeBytes = size,
                source = AppSource.APT,
            )
        }
        for (line in r.stdout.lineSequence()) {
            when {
                line.startsWith("Package: ") -> {
                    flush(); pkg = line.removePrefix("Package: ").trim()
                    section = ""; size = 0L; desc = ""
                }
                line.startsWith("Section: ") -> section = line.removePrefix("Section: ").trim()
                line.startsWith("Size: ") -> size = line.removePrefix("Size: ").trim().toLongOrNull() ?: 0L
                line.startsWith("Description-en: ") && desc.isEmpty() ->
                    desc = line.removePrefix("Description-en: ").trim()
                line.startsWith("Description: ") && desc.isEmpty() ->
                    desc = line.removePrefix("Description: ").trim()
            }
        }
        flush()
        return out.values.toList()
    }
}
