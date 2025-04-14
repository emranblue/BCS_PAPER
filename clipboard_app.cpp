#include <QApplication>
#include <QClipboard>
#include <QTimer>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QWidget>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QSqlError>
#include <QDebug>
#include <QPushButton>
#include <QProcess>
#include <QMessageBox>
#include <QRegularExpression>

QString createDictionaryInfo(const QString &word) {
    return QString("Synonyms: [placeholder], Antonyms: [placeholder], Example: \"%1 is a sample sentence.\"").arg(word);
}

bool wordExistsInDB(const QString &word, QString &translation, QString &dictionaryInfo) {
    QSqlQuery query;
    query.prepare("SELECT translation, dictionary_info FROM translations WHERE word = ?");
    query.addBindValue(word);
    if (query.exec() && query.next()) {
        translation = query.value(0).toString();
        dictionaryInfo = query.value(1).toString();
        return true;
    }
    return false;
}

void saveToDB(const QString &word, const QString &translation, const QString &dictionaryInfo) {
    QSqlQuery query;
    query.prepare("INSERT INTO translations (word, translation, dictionary_info) VALUES (?, ?, ?)");
    query.addBindValue(word);
    query.addBindValue(translation);
    query.addBindValue(dictionaryInfo);
    if (!query.exec()) {
        qWarning() << "DB insert failed:" << query.lastError().text();
    }
}

bool isSingleWord(const QString &text) {
    // Check if the text contains only letters and no whitespace or punctuation
    static QRegularExpression wordRegex("^[a-zA-Z]+$");
    return wordRegex.match(text).hasMatch();
}

QString cleanWord(const QString &word) {
    QString cleaned = word;
    cleaned.remove(QRegularExpression("[,.:'\"]")); // Remove common punctuation marks
    return cleaned.trimmed().toLower(); // Convert to lowercase
}

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QWidget window;
    window.setWindowTitle("Clipboard to Bangla");

    QVBoxLayout *mainLayout = new QVBoxLayout(&window);

    // Button layout
    QHBoxLayout *buttonLayout = new QHBoxLayout();
    QPushButton *extractButton = new QPushButton("Start Extract");
    QPushButton *mergeButton = new QPushButton("Confirm Merge");
    buttonLayout->addWidget(extractButton);
    buttonLayout->addWidget(mergeButton);
    mainLayout->addLayout(buttonLayout);

    QTextEdit *output = new QTextEdit;
    output->setReadOnly(true);
    mainLayout->addWidget(output);

    QClipboard *clipboard = QApplication::clipboard();
    QString lastText = clipboard->text().trimmed();
    QTimer timer;
    QNetworkAccessManager manager;

    // Database init
    QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE");
    db.setDatabaseName("translations.db");
    if (!db.open()) {
        qWarning() << "Failed to open DB!";
        return -1;
    }

    // Create table if not exists
    QSqlQuery initQuery;
    initQuery.exec("CREATE TABLE IF NOT EXISTS translations ("
                   "id INTEGER PRIMARY KEY, "
                   "word TEXT UNIQUE, "
                   "translation TEXT, "
                   "dictionary_info TEXT)");

    // Connect extract button
    QObject::connect(extractButton, &QPushButton::clicked, [&]() {
        QProcess *process = new QProcess(&window);
        process->start("./extract_new_words", QStringList());

        QObject::connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            [&window, process](int exitCode, QProcess::ExitStatus exitStatus) {
                if (exitStatus == QProcess::NormalExit && exitCode == 0) {
                    QMessageBox::information(&window, "Success", "Extraction completed successfully!");
                } else {
                    QMessageBox::warning(&window, "Error", "Extraction failed!");
                }
                process->deleteLater();
            });
    });

    // Connect merge button
    QObject::connect(mergeButton, &QPushButton::clicked, [&]() {
        QProcess *process = new QProcess(&window);
        process->start("./merge_and_update", QStringList());

        QObject::connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            [&window, process](int exitCode, QProcess::ExitStatus exitStatus) {
                if (exitStatus == QProcess::NormalExit && exitCode == 0) {
                    QMessageBox::information(&window, "Success", "Merge completed successfully!");
                } else {
                    QMessageBox::warning(&window, "Error", "Merge failed!");
                }
                process->deleteLater();
            });
    });

    QObject::connect(&timer, &QTimer::timeout, [&]() {
        QString currentText = cleanWord(clipboard->text().trimmed());
        if (!currentText.isEmpty() && currentText != lastText) {
            lastText = currentText;

            // Skip processing if it's not a single word
            if (!isSingleWord(currentText)) {
                output->append("⚠️ [Rejected]");
                output->append("🔹 Text: " + currentText);
                output->append("🔸 Reason: Only single words are accepted (no spaces/sentences)");
                output->append("");
                return;
            }

            QString fromDB_translation, fromDB_info;
            if (wordExistsInDB(currentText, fromDB_translation, fromDB_info)) {
                output->append("📁 [From DB]");
                output->append("🔹 Word: " + currentText);
                output->append("🔸 বাংলা: " + fromDB_translation);
                output->append("📚 Dictionary: " + fromDB_info);
                output->append("");
            } else {
                QString url = QString("https://translate.googleapis.com/translate_a/single"
                      "?client=gtx&sl=en&tl=bn&dt=t&q=%1")
                      .arg(QString(QUrl::toPercentEncoding(currentText)));

                QNetworkRequest request{QUrl(url)};
                QNetworkReply *reply = manager.get(request);

                QObject::connect(reply, &QNetworkReply::finished, [&, currentText, reply]() {
                    if (reply->error() != QNetworkReply::NoError) {
                        output->append("⚠️ [Network Error]");
                        output->append("🔹 Word: " + currentText);
                        output->append("🔸 বাংলা: [Not available - no network]");
                        output->append("📚 Dictionary: [Not available]");
                        output->append("");
                        saveToDB(currentText, "[Not available - no network]", "[Not available]");
                    } else {
                        QByteArray response = reply->readAll();
                        QJsonDocument doc = QJsonDocument::fromJson(response);

                        if (doc.isArray() && !doc.array().isEmpty() && doc.array()[0].isArray()) {
                            QJsonArray arr = doc.array()[0].toArray();
                            QString translated;
                            for (const QJsonValue &val : arr) {
                                if (val.isArray() && !val.toArray().isEmpty()) {
                                    translated += val.toArray()[0].toString();
                                }
                            }

                            QString dictionaryInfo = createDictionaryInfo(currentText);

                            output->append("🌐 [From API]");
                            output->append("🔹 Word: " + currentText);
                            output->append("🔸 বাংলা: " + translated);
                            output->append("📚 Dictionary: " + dictionaryInfo);
                            output->append("");

                            saveToDB(currentText, translated, dictionaryInfo);
                        } else {
                            output->append("⚠️ [API Error]");
                            output->append("🔹 Word: " + currentText);
                            output->append("🔸 বাংলা: [Translation failed]");
                            output->append("📚 Dictionary: [Not available]");
                            output->append("");
                            saveToDB(currentText, "[Translation failed]", "[Not available]");
                        }
                    }
                    reply->deleteLater();
                });
            }
        }
    });

    timer.start(1000);
    window.resize(500, 400);
    window.show();
    return app.exec();
}
