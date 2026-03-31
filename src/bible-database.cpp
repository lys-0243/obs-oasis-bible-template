#include "bible-database.h"
#include <stdexcept>

// ---------------------------------------------------------------------------
// Helpers internes (macros de guard)
// ---------------------------------------------------------------------------

#define DB_CHECK(expr)          \
    do {                        \
        if (!(expr)) return {}; \
    } while (0)

// ---------------------------------------------------------------------------
// Ctor / Dtor
// ---------------------------------------------------------------------------

BibleDatabase::BibleDatabase() = default;

BibleDatabase::~BibleDatabase()
{
    close();
}

// ---------------------------------------------------------------------------
// Cycle de vie
// ---------------------------------------------------------------------------

bool BibleDatabase::open(const std::string &path)
{
    if (db_)
        close();

    int rc = sqlite3_open(path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        db_ = nullptr;
        return false;
    }

    // Performances : WAL + clés étrangères
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA foreign_keys=ON;");
    exec("PRAGMA synchronous=NORMAL;");

    return initSchema();
}

void BibleDatabase::close()
{
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// Schéma
// ---------------------------------------------------------------------------

bool BibleDatabase::initSchema()
{
    const char *sql = R"(
        CREATE TABLE IF NOT EXISTS translations (
            id   INTEGER PRIMARY KEY AUTOINCREMENT,
            code TEXT UNIQUE NOT NULL,
            name TEXT NOT NULL
        );

        CREATE TABLE IF NOT EXISTS books (
            id             INTEGER PRIMARY KEY AUTOINCREMENT,
            translation_id INTEGER NOT NULL REFERENCES translations(id),
            number         INTEGER NOT NULL,
            name           TEXT    NOT NULL,
            testament      TEXT    NOT NULL CHECK(testament IN ('OT','NT')),
            UNIQUE(translation_id, number)
        );

        CREATE TABLE IF NOT EXISTS verses (
            id       INTEGER PRIMARY KEY AUTOINCREMENT,
            book_id  INTEGER NOT NULL REFERENCES books(id),
            chapter  INTEGER NOT NULL,
            verse    INTEGER NOT NULL,
            text     TEXT    NOT NULL,
            UNIQUE(book_id, chapter, verse)
        );

        CREATE INDEX IF NOT EXISTS idx_verses_lookup
            ON verses(book_id, chapter, verse);

        CREATE INDEX IF NOT EXISTS idx_verses_text
            ON verses(text);
    )";

    return exec(sql);
}

// ---------------------------------------------------------------------------
// Traductions
// ---------------------------------------------------------------------------

std::vector<Translation> BibleDatabase::getTranslations() const
{
    std::vector<Translation> result;
    if (!db_) return result;

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT id, code, name FROM translations ORDER BY code;",
            -1, &stmt, nullptr) != SQLITE_OK)
        return result;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Translation t;
        t.id   = sqlite3_column_int(stmt, 0);
        t.code = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
        t.name = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 2));
        result.push_back(std::move(t));
    }
    sqlite3_finalize(stmt);
    return result;
}

int BibleDatabase::addTranslation(const std::string &code,
                                   const std::string &name)
{
    if (!db_) return -1;

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT OR IGNORE INTO translations(code, name) VALUES(?,?);",
            -1, &stmt, nullptr) != SQLITE_OK)
        return -1;

    sqlite3_bind_text(stmt, 1, code.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    // Récupère l'id (qu'il vienne d'un INSERT ou d'un IGNORE)
    stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT id FROM translations WHERE code=?;",
            -1, &stmt, nullptr) != SQLITE_OK)
        return -1;

    sqlite3_bind_text(stmt, 1, code.c_str(), -1, SQLITE_TRANSIENT);
    int id = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return id;
}

// ---------------------------------------------------------------------------
// Livres
// ---------------------------------------------------------------------------

std::vector<Book> BibleDatabase::getBooks(int translationId) const
{
    std::vector<Book> result;
    if (!db_) return result;

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT id, translation_id, number, name, testament "
            "FROM books WHERE translation_id=? ORDER BY number;",
            -1, &stmt, nullptr) != SQLITE_OK)
        return result;

    sqlite3_bind_int(stmt, 1, translationId);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Book b;
        b.id            = sqlite3_column_int(stmt, 0);
        b.translationId = sqlite3_column_int(stmt, 1);
        b.number        = sqlite3_column_int(stmt, 2);
        b.name          = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
        b.testament     = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
        result.push_back(std::move(b));
    }
    sqlite3_finalize(stmt);
    return result;
}

int BibleDatabase::getChapterCount(int bookId) const
{
    if (!db_) return 0;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT MAX(chapter) FROM verses WHERE book_id=?;",
            -1, &stmt, nullptr) != SQLITE_OK)
        return 0;
    sqlite3_bind_int(stmt, 1, bookId);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

int BibleDatabase::getVerseCount(int bookId, int chapter) const
{
    if (!db_) return 0;
    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT MAX(verse) FROM verses WHERE book_id=? AND chapter=?;",
            -1, &stmt, nullptr) != SQLITE_OK)
        return 0;
    sqlite3_bind_int(stmt, 1, bookId);
    sqlite3_bind_int(stmt, 2, chapter);
    int count = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        count = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return count;
}

// ---------------------------------------------------------------------------
// Récupération de versets
// ---------------------------------------------------------------------------

std::optional<Verse> BibleDatabase::getVerse(int bookId,
                                               int chapter,
                                               int verse) const
{
    if (!db_) return std::nullopt;

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT v.id, v.book_id, v.chapter, v.verse, v.text, "
            "       b.name, t.code "
            "FROM verses v "
            "JOIN books b ON b.id = v.book_id "
            "JOIN translations t ON t.id = b.translation_id "
            "WHERE v.book_id=? AND v.chapter=? AND v.verse=? "
            "LIMIT 1;",
            -1, &stmt, nullptr) != SQLITE_OK)
        return std::nullopt;

    sqlite3_bind_int(stmt, 1, bookId);
    sqlite3_bind_int(stmt, 2, chapter);
    sqlite3_bind_int(stmt, 3, verse);

    std::optional<Verse> result;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        Verse v;
        v.id              = sqlite3_column_int(stmt, 0);
        v.bookId          = sqlite3_column_int(stmt, 1);
        v.chapter         = sqlite3_column_int(stmt, 2);
        v.verse           = sqlite3_column_int(stmt, 3);
        v.text            = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
        v.bookName        = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
        v.translationCode = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6));
        result = std::move(v);
    }
    sqlite3_finalize(stmt);
    return result;
}

std::vector<Verse> BibleDatabase::getChapter(int bookId, int chapter) const
{
    std::vector<Verse> result;
    if (!db_) return result;

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT v.id, v.book_id, v.chapter, v.verse, v.text, "
            "       b.name, t.code "
            "FROM verses v "
            "JOIN books b ON b.id = v.book_id "
            "JOIN translations t ON t.id = b.translation_id "
            "WHERE v.book_id=? AND v.chapter=? "
            "ORDER BY v.verse;",
            -1, &stmt, nullptr) != SQLITE_OK)
        return result;

    sqlite3_bind_int(stmt, 1, bookId);
    sqlite3_bind_int(stmt, 2, chapter);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Verse v;
        v.id              = sqlite3_column_int(stmt, 0);
        v.bookId          = sqlite3_column_int(stmt, 1);
        v.chapter         = sqlite3_column_int(stmt, 2);
        v.verse           = sqlite3_column_int(stmt, 3);
        v.text            = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
        v.bookName        = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
        v.translationCode = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6));
        result.push_back(std::move(v));
    }
    sqlite3_finalize(stmt);
    return result;
}

// ---------------------------------------------------------------------------
// Recherche par mot-clé
// ---------------------------------------------------------------------------

std::vector<Verse> BibleDatabase::searchByKeyword(int translationId,
                                                    const std::string &keyword,
                                                    int maxResults) const
{
    std::vector<Verse> result;
    if (!db_ || keyword.empty()) return result;

    std::string pattern = "%" + keyword + "%";

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT v.id, v.book_id, v.chapter, v.verse, v.text, "
            "       b.name, t.code "
            "FROM verses v "
            "JOIN books b ON b.id = v.book_id "
            "JOIN translations t ON t.id = b.translation_id "
            "WHERE b.translation_id=? AND v.text LIKE ? "
            "ORDER BY b.number, v.chapter, v.verse "
            "LIMIT ?;",
            -1, &stmt, nullptr) != SQLITE_OK)
        return result;

    sqlite3_bind_int(stmt, 1, translationId);
    sqlite3_bind_text(stmt, 2, pattern.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, maxResults);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        Verse v;
        v.id              = sqlite3_column_int(stmt, 0);
        v.bookId          = sqlite3_column_int(stmt, 1);
        v.chapter         = sqlite3_column_int(stmt, 2);
        v.verse           = sqlite3_column_int(stmt, 3);
        v.text            = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 4));
        v.bookName        = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 5));
        v.translationCode = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 6));
        result.push_back(std::move(v));
    }
    sqlite3_finalize(stmt);
    return result;
}

// ---------------------------------------------------------------------------
// Import en masse
// ---------------------------------------------------------------------------

int BibleDatabase::addBook(int translationId, int number,
                            const std::string &name,
                            const std::string &testament)
{
    if (!db_) return -1;

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT OR IGNORE INTO books(translation_id, number, name, testament) "
            "VALUES(?,?,?,?);",
            -1, &stmt, nullptr) != SQLITE_OK)
        return -1;

    sqlite3_bind_int(stmt, 1, translationId);
    sqlite3_bind_int(stmt, 2, number);
    sqlite3_bind_text(stmt, 3, name.c_str(),      -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, testament.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "SELECT id FROM books WHERE translation_id=? AND number=?;",
            -1, &stmt, nullptr) != SQLITE_OK)
        return -1;

    sqlite3_bind_int(stmt, 1, translationId);
    sqlite3_bind_int(stmt, 2, number);
    int id = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW)
        id = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return id;
}

bool BibleDatabase::insertVersesBatch(const std::vector<Verse> &verses)
{
    if (!db_ || verses.empty()) return false;

    // Transaction unique = 100x plus rapide qu'INSERT par INSERT
    if (!exec("BEGIN TRANSACTION;"))
        return false;

    sqlite3_stmt *stmt = nullptr;
    if (sqlite3_prepare_v2(db_,
            "INSERT OR IGNORE INTO verses(book_id, chapter, verse, text) "
            "VALUES(?,?,?,?);",
            -1, &stmt, nullptr) != SQLITE_OK) {
        exec("ROLLBACK;");
        return false;
    }

    for (const Verse &v : verses) {
        sqlite3_bind_int(stmt, 1, v.bookId);
        sqlite3_bind_int(stmt, 2, v.chapter);
        sqlite3_bind_int(stmt, 3, v.verse);
        sqlite3_bind_text(stmt, 4, v.text.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(stmt);
        sqlite3_reset(stmt);
    }

    sqlite3_finalize(stmt);
    exec("COMMIT;");
    return true;
}

// ---------------------------------------------------------------------------
// Utilitaires
// ---------------------------------------------------------------------------

std::string BibleDatabase::lastError() const
{
    if (!db_) return "Database not open";
    return sqlite3_errmsg(db_);
}

bool BibleDatabase::exec(const char *sql) const
{
    char *errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg);
    if (errMsg) sqlite3_free(errMsg);
    return rc == SQLITE_OK;
}
