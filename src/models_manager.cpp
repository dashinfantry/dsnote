/* Copyright (C) 2021 Michal Kosciesza <michal@mkiol.net>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "models_manager.h"

#include <QDebug>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonArray>
#include <QJsonObject>
#include <QByteArray>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QCryptographicHash>
#include <QVariantList>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>

#include <fstream>
#include <utility>
#include <vector>
#include <iostream>
#include <string>
#include <functional>

#include "settings.h"
#include "info.h"

const QString models_manager::lang_models_file{"lang_models.json"};

models_manager::models_manager(QObject *parent)
    : QObject(parent)
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
    nam.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
#endif

    connect(settings::instance(), &settings::lang_models_dir_changed, this,
            std::bind(&models_manager::parse_models_file, this, false));

    parse_models_file();
}

bool models_manager::ok() const
{
    return !models.empty();
}

std::vector<models_manager::lang_t> models_manager::langs() const
{
    std::vector<lang_t> list;

    QDir dir{settings::instance()->lang_models_dir()};

    std::transform(models.cbegin(), models.cend(), std::back_inserter(list),
                [&dir](decltype(models)::value_type const& pair) {
        return lang_t{
            pair.first,
            pair.second.name,
            pair.second.scorer_file_name.isEmpty() ? "" : dir.filePath(pair.second.scorer_file_name),
            pair.second.scorer_file_name.isEmpty() ? "" : dir.filePath(pair.second.scorer_file_name),
            pair.second.available,
            pair.second.current_dl != download_type::none
        };
    });

    return list;
}

std::vector<models_manager::lang_t> models_manager::available_langs() const
{
    std::vector<lang_t> list;

    QDir dir{settings::instance()->lang_models_dir()};

    for (const auto& [id, model] : models) {
        const auto model_file = dir.filePath(model.file_name);
        if (model.available && QFile::exists(model_file))
            list.push_back({id,
                            model.name,
                            model_file,
                            model.scorer_file_name.isEmpty() ? "" : dir.filePath(model.scorer_file_name),
                            model.available,
                            model.current_dl != download_type::none});
    }

    return list;
}

bool models_manager::model_exists(const QString& id) const
{
    const auto it = models.find(id);

    return it != std::cend(models) &&
            QFile::exists(model_path(it->second.file_name));
}

void models_manager::download_model(const QString& id)
{
    download(id, download_type::all);
}

void models_manager::download(const QString& id, download_type type)
{
    if (auto it = models.find(id); it != std::end(models)) {
        auto& model = it->second;

        QNetworkReply* reply;
        QString path;
        QString md5;
        qint64 size;
        auto next_type = download_type::none;
        if (type == download_type::all) {
            QNetworkRequest request{model.url};
            request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
            reply = nam.get(request);
            path = model_path(model.file_name);
            md5 = model.md5;
            size = model.size;
            type = download_type::model;
            if (!model.scorer_file_name.isEmpty() && !model.scorer_md5.isEmpty() && !model.scorer_url.isEmpty()) {
                next_type = download_type::scorer;
            }
            model.current_dl = next_type != download_type::none ? download_type::all : download_type::model;
        } else if (type == download_type::scorer) {
            QNetworkRequest request{model.scorer_url};
            request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
            reply = nam.get(request);
            path = model_path(model.scorer_file_name);
            md5 = model.scorer_md5;
            size = model.scorer_size;
            next_type = download_type::none;
            model.current_dl = download_type::all;
        } else {
            qWarning() << "Incorrect dl type requested:" << static_cast<int>(type);
            return;
        }

        auto out_file = new std::ofstream{path.toStdString(), std::ofstream::out};

        reply->setProperty("out_file", QVariant::fromValue(static_cast<void*>(out_file)));
        reply->setProperty("out_path", path);
        reply->setProperty("lang_id", id);
        reply->setProperty("download_type", static_cast<int>(type));
        reply->setProperty("download_next_type", static_cast<int>(next_type));
        reply->setProperty("checksum", md5);
        reply->setProperty("size", size);

        connect(reply, &QNetworkReply::downloadProgress, this, &models_manager::handle_download_progress);
        connect(reply, &QNetworkReply::finished, this, &models_manager::handle_download_finished);
        connect(reply, &QNetworkReply::readyRead, this, &models_manager::handle_download_ready_read);

        emit models_changed();
    } else {
        qWarning() << "no model with id:" << id;
    }
}

void models_manager::handle_download_ready_read()
{
    auto reply = qobject_cast<QNetworkReply*>(sender());

    if (reply->bytesAvailable() > 0) {
        auto data = reply->readAll();
        auto out_file = static_cast<std::ofstream*>(reply->property("out_file").value<void*>());
        out_file->write(data.data(), data.size());
    }
}

void models_manager::handle_download_finished()
{
    auto reply = qobject_cast<QNetworkReply*>(sender());

    const auto id = reply->property("lang_id").toString();
    const auto type = static_cast<download_type>(reply->property("download_type").toInt());

    delete static_cast<std::ofstream*>(reply->property("out_file").value<void*>());

    if (reply->error() != QNetworkReply::NoError) {
        qWarning() << "error in download:" << (type == download_type::scorer ? "scorer" : "model") << id;
        emit download_error(id);
    } else {
        auto path = reply->property("out_path").toString();

        if (md5(path) == reply->property("checksum").toString()) {
            qDebug() << "successfully downloaded:" << (type == download_type::scorer ? "scorer" : "model") << id;

            if (static_cast<download_type>(reply->property("download_next_type").toInt()) == download_type::scorer) {
                download(id, download_type::scorer);
                reply->deleteLater();
                return;
            } else {
                models[id].available = true;
                emit download_finished(id);
            }
        } else {
            qWarning() << "checksum is invalid:" << (type == download_type::scorer ? "scorer" : "model") << id;
            QFile::remove(path);
            emit download_error(id);
        }
    }

    reply->deleteLater();

    models[id].current_dl = download_type::none;
    emit models_changed();
}

QString models_manager::md5(const QString& file)
{
    if (std::ifstream input{file.toStdString(), std::ios::in | std::ifstream::binary}) {
        QCryptographicHash hash{QCryptographicHash::Md5};

        while (input) {
            char buff[std::numeric_limits<unsigned short>::max()];
            input.read(buff, sizeof buff);
            hash.addData(buff, static_cast<int>(input.gcount()));
        }

        return hash.result().toHex();
    }

    return {};
}

void models_manager::handle_download_progress(qint64 received, qint64 total)
{
    const auto reply = qobject_cast<const QNetworkReply*>(sender());
    const auto id = reply->property("lang_id").toString();
    const auto type = static_cast<download_type>(reply->property("download_type").toInt());

    if (total <= 0)
        total = reply->property("size").toLongLong();

    qDebug("%s download progress for %s: %lld / %lld",
           (type == download_type::scorer ? "scorer" : "model"),
           id.toLatin1().data(), received, total);

    if (total > 0) {
        double progress;
        if (models.at(id).current_dl == download_type::all) {
            progress = static_cast<double>(received)/(2 * total) + (type == download_type::scorer ? 0.5 : 0.0);
        } else {
            progress = static_cast<double>(received)/total;
        }

        emit download_progress(id, progress);
    }
}

void models_manager::delete_model(const QString &id)
{
    if (const auto it = models.find(id); it != std::cend(models)) {
        auto& model = it->second;
        QFile::remove(model_path(model.file_name));
        QFile::remove(model_path(model.scorer_file_name));
        model.available = false;
        emit models_changed();
    } else {
        qWarning() << "no model with id:" << id;
    }
}

void models_manager::backup_config(const QString& lang_models_file)
{
    QString backup_file;
    QFileInfo lang_models_file_i{lang_models_file};
    auto backup_file_stem = lang_models_file_i.dir().filePath(lang_models_file_i.baseName());

    int i = 0;
    do {
        backup_file = backup_file_stem + ".old" + (i == 0 ? "" : QString::number(i)) + ".json";
        ++i;
    } while (QFile::exists(backup_file) && i < 1000);

    qDebug() << "making lang models file backup to:" << backup_file;

    QFile::remove(backup_file);
    QFile::copy(lang_models_file, backup_file);
}

void models_manager::init_config()
{
    const QString data_dir{QStandardPaths::writableLocation(QStandardPaths::DataLocation)};
    const auto lang_models_file_path = QDir{data_dir}.filePath(lang_models_file);

    if (QFile::exists(lang_models_file_path))
        backup_config(lang_models_file_path);

    QDir{}.mkpath(data_dir);

    /*
     M - mandatory
     O - optional

     { "version": <config_version>,
       "langs: [
       {
         "name": "<native language name (M)>",
         "id": "<ISO 639-1 language code (M)>",
         "file_name": "<file name of ds model (O)>",
         "md5": "<md5 hash of ds model file (M)>",
         "url": "<download URL of ds model file (M)>",
         "size": "<size in bytes of ds model file (O)>",
         "scorer_file_name": "<file name of ds model (O)>",
         "scorer_md5": "<md5 hash of ds model file (M if scorer is provided)>",
         "scorer_url": "<download URL of ds model file (O)>",
         "scorer_size": "<size in bytes of ds model file (O)>"
       } ]
    }
    */

    std::ofstream outfile{lang_models_file_path.toStdString(), std::ofstream::out | std::ofstream::trunc};
    outfile << "{\n\"version\": " << dsnote::CONF_VERSION << ",\n\"langs\": [\n"

#ifdef TF_LITE
            << "{ \"name\": \"English\", \"id\": \"en\", "
            << "\"file_name\": \"en.tflite\", \"md5\": \"afcc08e56f024655c30187a41c4e8c9c\", \"url\": \"https://github.com/mozilla/DeepSpeech/releases/download/v0.9.3/deepspeech-0.9.3-models.tflite\", \"size\": \"47331784\", "
            << "\"scorer_file_name\": \"en.scorer\", \"scorer_md5\": \"08a02b383a9bc93c8a8ad188dbf79bc9\", \"scorer_url\": \"https://github.com/mozilla/DeepSpeech/releases/download/v0.9.3/deepspeech-0.9.3-models.scorer\", \"scorer_size\": \"953363776\"},\n"

            << "{ \"name\": \"Deutsch\", \"id\": \"de\", "
            << "\"file_name\": \"de.tflite\", \"md5\": \"bc31379c31052392b2ea881eefede747\", \"url\": \"\", \"size\": \"47332112\", "
            << "\"scorer_file_name\": \"de.scorer\", \"scorer_md5\": \"e1fbc58d92c0872f7a1502d33416a23c\", \"scorer_url\": \"\", \"scorer_size\": \"279121440\"},\n"

            << "{ \"name\": \"Español\", \"id\": \"es\", "
            << "\"file_name\": \"es.tflite\", \"md5\": \"cc618b45dd01b8a6cc6b1d781653f89a\", \"url\": \"\", \"size\": \"47346520\", "
            << "\"scorer_file_name\": \"es.scorer\", \"scorer_md5\": \"650e2325ebf70d08a69ae5bf238ad5bd\", \"scorer_url\": \"\", \"scorer_size\": \"279655392\"},\n"

            << "{ \"name\": \"Français\", \"id\": \"fr\", "
            << "\"file_name\": \"fr.tflite\", \"md5\": \"fcf644611a833f4f8e9767b2ab6b16ea\", \"url\": \"\", \"size\": \"47365040\", "
            << "\"scorer_file_name\": \"fr.scorer\", \"scorer_md5\": \"35224069b08e801c84051d65e810bdd1\", \"scorer_url\": \"\", \"scorer_size\": \"254972864\"},\n"

            << "{ \"name\": \"Italiano\", \"id\": \"it\", "
            << "\"file_name\": \"it.tflite\", \"md5\": \"9b2df256185e442246159b33cd05fc2d\", \"url\": \"\", \"size\": \"47332112\", "
            << "\"scorer_file_name\": \"it.scorer\", \"scorer_md5\": \"08a02b383a9bc93c8a8ad188dbf79bc9\", \"scorer_url\": \"\", \"scorer_size\": \"7746880\"},\n"

            << "{ \"name\": \"Polski\", \"id\": \"pl\", "
            << "\"file_name\": \"pl.tflite\", \"md5\": \"a56c693bb0d644af5dc53f0e59f0da76\", \"url\": \"\", \"size\": \"47348576\", "
            << "\"scorer_file_name\": \"pl.scorer\", \"scorer_md5\": \"0984ebda29d9c51a87e5823bd301d980\", \"scorer_url\": \"\", \"scorer_size\": \"4856464\"},\n"

            << "{ \"name\": \"简体中文\", \"id\": \"zh-CN\", "
            << "\"file_name\": \"zh-CN.tflite\", \"md5\": \"5664793cafe796d0821a3e49d56eb797\", \"url\": \"https://github.com/mozilla/DeepSpeech/releases/download/v0.9.3/deepspeech-0.9.3-models-zh-CN.tflite\", \"size\": \"47798728\", "
            << "\"scorer_file_name\": \"zh-CN.scorer\", \"scorer_md5\": \"628e68fd8e0dd82c4a840d56c4cdc661\", \"scorer_url\": \"https://github.com/mozilla/DeepSpeech/releases/download/v0.9.3/deepspeech-0.9.3-models-zh-CN.scorer\", \"scorer_size\": \"67141744\"}\n"
#else
            << "{ \"name\": \"English\", \"id\": \"en\", "
            << "\"file_name\": \"en.pbmm\", \"md5\": \"8b15ccb86d0214657e48371287b7a49a\", \"url\": \"https://github.com/mozilla/DeepSpeech/releases/download/v0.9.3/deepspeech-0.9.3-models.pbmm\", \"size\": \"188915987\", "
            << "\"scorer_file_name\": \"en.scorer\", \"scorer_md5\": \"08a02b383a9bc93c8a8ad188dbf79bc9\", \"scorer_url\": \"https://github.com/mozilla/DeepSpeech/releases/download/v0.9.3/deepspeech-0.9.3-models.scorer\", \"scorer_size\": \"953363776\"},\n"

            << "{ \"name\": \"Deutsch\", \"id\": \"de\", "
            << "\"file_name\": \"de.pbmm\", \"md5\": \"ccb15318053a245487a15b90bf052cca\", \"url\": \"\", \"size\": \"188916323\", "
            << "\"scorer_file_name\": \"de.scorer\", \"scorer_md5\": \"e1fbc58d92c0872f7a1502d33416a23c\", \"scorer_url\": \"\", \"scorer_size\": \"279121440\"},\n"

            << "{ \"name\": \"Español\", \"id\": \"es\", "
            << "\"file_name\": \"es.pbmm\", \"md5\": \"8b0739839abd0f98f2638be166fb3b74\", \"url\": \"\", \"size\": \"188973740\", "
            << "\"scorer_file_name\": \"es.scorer\", \"scorer_md5\": \"650e2325ebf70d08a69ae5bf238ad5bd\", \"scorer_url\": \"\", \"scorer_size\": \"279655392\"},\n"

            << "{ \"name\": \"Français\", \"id\": \"fr\", "
            << "\"file_name\": \"fr.pbmm\", \"md5\": \"079fa68c49feff6aa2bd3cc22aab6226\", \"url\": \"\", \"size\": \"189047557\", "
            << "\"scorer_file_name\": \"fr.scorer\", \"scorer_md5\": \"35224069b08e801c84051d65e810bdd1\", \"scorer_url\": \"\", \"scorer_size\": \"254972864\"},\n"

            << "{ \"name\": \"Italiano\", \"id\": \"it\", "
            << "\"file_name\": \"it.pbmm\", \"md5\": \"ec10ea9d01cc9ab3135e4e5b0341821e\", \"url\": \"\", \"size\": \"188916323\", "
            << "\"scorer_file_name\": \"it.scorer\", \"scorer_md5\": \"08a02b383a9bc93c8a8ad188dbf79bc9\", \"scorer_url\": \"\", \"scorer_size\": \"7746880\"},\n"

            << "{ \"name\": \"Polski\", \"id\": \"pl\", "
            << "\"file_name\": \"pl.pbmm\", \"md5\": \"69d0069a0d68f33f6634e8b2c0e06af6\", \"url\": \"\", \"size\": \"188981942\", "
            << "\"scorer_file_name\": \"pl.scorer\", \"scorer_md5\": \"0984ebda29d9c51a87e5823bd301d980\", \"scorer_url\": \"\", \"scorer_size\": \"4856464\"},\n"

            << "{ \"name\": \"简体中文\", \"id\": \"zh-CN\", "
            << "\"file_name\": \"zh-CN.pbmm\", \"md5\": \"57b99451aaabbada2708e3b6a28e55c8\", \"url\": \"https://github.com/mozilla/DeepSpeech/releases/download/v0.9.3/deepspeech-0.9.3-models-zh-CN.pbmm\", \"size\": \"190777619\", "
            << "\"scorer_file_name\": \"zh-CN.scorer\", \"scorer_md5\": \"628e68fd8e0dd82c4a840d56c4cdc661\", \"scorer_url\": \"https://github.com/mozilla/DeepSpeech/releases/download/v0.9.3/deepspeech-0.9.3-models-zh-CN.scorer\", \"scorer_size\": \"67141744\"}\n"
#endif
            << "]\n}\n";
    outfile.close();
}

void models_manager::parse_models_file(bool reset)
{
    const auto lang_models_file_path = QDir{QStandardPaths::writableLocation(QStandardPaths::DataLocation)}
                                           .filePath(lang_models_file);

    if (!QFile::exists(lang_models_file_path))
        init_config();

    if (std::ifstream input{lang_models_file_path.toStdString(), std::ifstream::in | std::ifstream::binary}) {
        const std::vector<char> buff{std::istreambuf_iterator<char>{input}, {}};

        QJsonParseError err;
        auto json = QJsonDocument::fromJson(QByteArray::fromRawData(&buff[0], buff.size()), &err);
        if (err.error != QJsonParseError::NoError) {
            qWarning() << "error parsing json:" << err.errorString();
            input.close();
            if (!reset) {
                init_config();
                parse_models_file(true);
            }
            return;
        }

        auto version = json.object().value("version").toInt();

        if (version < dsnote::CONF_VERSION) {
            qWarning("version mismatch, has %d but requires %d", version, dsnote::CONF_VERSION);
            input.close();
            if (!reset) {
                init_config();
                parse_models_file(true);
            }
            return;
        }

        models.clear();

        QDir dir{settings::instance()->lang_models_dir()};

        for (const auto ele : json.object().value("langs").toArray()) {
            auto obj = ele.toObject();
            auto id = obj.value("id").toString();

            if (id.isEmpty()) {
                qWarning() << "empty id in lang models file";
                continue;
            }

            if (models.find(id) != models.end()) {
                qWarning() << "duplicate id in lang models file:" << id;
                continue;
            }

            auto md5 = obj.value("md5").toString();

            if (md5.isEmpty()) {
                qWarning() << "md5 checksum cannot be empty:" << id;
                continue;
            }

            auto file_name = obj.value("file_name").toString();
            if (file_name.isEmpty())
                file_name = file_name_from_id(id);

            auto scorer_md5 = obj.value("scorer_md5").toString();
            auto scorer_url = QUrl{obj.value("scorer_url").toString()};
            auto scorer_file_name = obj.value("scorer_file_name").toString();
            if (scorer_file_name.isEmpty())
                scorer_file_name = scorer_file_name_from_id(id);

            bool available =
                    dir.exists(file_name) &&
                    md5 == this->md5(dir.filePath(file_name)) &&
                    ((dir.exists(scorer_file_name) &&
                    scorer_md5 == this->md5(dir.filePath(scorer_file_name))) ||
                    scorer_url.isEmpty());

            models[id] = {
                obj.value("name").toString(),
                file_name,
                md5,
                QUrl{obj.value("url").toString()},
                obj.value("size").toString().toLongLong(),
                scorer_file_name,
                scorer_md5,
                scorer_url,
                obj.value("scorer_size").toString().toLongLong(),
                available,
                download_type::none
            };
        }
    } else {
        qWarning() << "cannot open lang models file";
    }

    emit models_changed();
}

QString models_manager::file_name_from_id(const QString& id)
{
#ifdef TF_LITE
    return id + ".tflite";
#else
    return id + ".pbmm";
#endif
}

QString models_manager::scorer_file_name_from_id(const QString& id)
{
    return id + ".scorer";
}

QString models_manager::model_path(const QString& file_name)
{
    return QDir{settings::instance()->lang_models_dir()}.filePath(file_name);
}