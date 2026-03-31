#pragma once

#include <string>
#include <vector>
#include <optional>
#include "sqlite3.h"

// ---------------------------------------------------------------------------
// Structures de données
// ---------------------------------------------------------------------------

struct Translation {
    int         id;
    std::string code;   // ex: "LSG", "NEG", "KJV"
    std::string name;   // ex: "Louis Segond 1910"
};

struct Book {
    int         id;
    int         translationId;
    int         number;     // 1 = Genèse ... 66 = Apocalypse
    std::string name;
    std::string testament;  // "OT" ou "NT"
};

struct Verse {
    int         id;
    int         bookId;
    int         chapter;
    int         verse;
    std::string text;
    // champs dénormalisés pour l'affichage (remplis par certaines requêtes)
    std::string bookName;
    std::string translationCode;
};

// ---------------------------------------------------------------------------
// BibleDatabase
// Gère l'ouverture, la création du schéma et toutes les requêtes SQLite.
// Thread-safety : à appeler depuis le thread UI uniquement.
// ---------------------------------------------------------------------------

class BibleDatabase {
public:
    BibleDatabase();
    ~BibleDatabase();

    // Non copiable
    BibleDatabase(const BibleDatabase &) = delete;
    BibleDatabase &operator=(const BibleDatabase &) = delete;

    // --- Cycle de vie -------------------------------------------------------

    // Ouvre (ou crée) la base au chemin indiqué.
    // Retourne true si succès.
    bool open(const std::string &path);

    // Ferme la connexion proprement.
    void close();

    bool isOpen() const { return db_ != nullptr; }

    // --- Gestion des traductions --------------------------------------------

    // Retourne toutes les traductions disponibles.
    std::vector<Translation> getTranslations() const;

    // Insère une traduction ; retourne son id ou -1 si erreur.
    int addTranslation(const std::string &code, const std::string &name);

    // --- Gestion des livres -------------------------------------------------

    // Retourne tous les livres d'une traduction, triés par number.
    std::vector<Book> getBooks(int translationId) const;

    // Nombre de chapitres dans un livre.
    int getChapterCount(int bookId) const;

    // Nombre de versets dans un chapitre.
    int getVerseCount(int bookId, int chapter) const;

    // --- Récupération de versets --------------------------------------------

    // Verset exact (référence précise).
    std::optional<Verse> getVerse(int bookId, int chapter, int verse) const;

    // Tous les versets d'un chapitre.
    std::vector<Verse> getChapter(int bookId, int chapter) const;

    // --- Recherche ----------------------------------------------------------

    // Recherche par mot-clé dans le texte (LIKE %keyword%).
    // Limité à maxResults résultats pour ne pas bloquer l'UI.
    std::vector<Verse> searchByKeyword(int translationId,
                                       const std::string &keyword,
                                       int maxResults = 50) const;

    // --- Import en masse ----------------------------------------------------

    // Insère un livre et retourne son id (-1 si erreur).
    int addBook(int translationId, int number,
                const std::string &name, const std::string &testament);

    // Insère un lot de versets dans une transaction unique (performant).
    // Chaque élément du vecteur : {bookId, chapter, verse, text}
    bool insertVersesBatch(const std::vector<Verse> &verses);

    // --- Utilitaires --------------------------------------------------------

    // Dernier message d'erreur SQLite.
    std::string lastError() const;

private:
    // Crée les tables si elles n'existent pas encore.
    bool initSchema();

    // Exécute un statement SQL simple sans résultat.
    bool exec(const char *sql) const;

    sqlite3 *db_ = nullptr;
};
