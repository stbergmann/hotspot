/*
  perfrecord.cpp

  This file is part of Hotspot, the Qt GUI for performance analysis.

  Copyright (C) 2017-2018 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com
  Author: Nate Rogers <nate.rogers@kdab.com>

  Licensees holding valid commercial KDAB Hotspot licenses may use this file in
  accordance with Hotspot Commercial License Agreement provided with the Software.

  Contact info@kdab.com if any conditions of this licensing are not clear to you.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "perfrecord.h"

#include <QDebug>
#include <QFileInfo>
#include <QProcess>
#include <QDir>
#include <QStandardPaths>

#include <csignal>

#include <KUser>
#include <KWindowSystem>

PerfRecord::PerfRecord(QObject* parent)
    : QObject(parent)
    , m_perfRecordProcess(nullptr)
    , m_outputPath()
    , m_userTerminated(false)
{
}

PerfRecord::~PerfRecord()
{
    if (m_perfRecordProcess) {
        stopRecording();
        m_perfRecordProcess->waitForFinished(100);
        delete m_perfRecordProcess;
    }
}

static QStringList sudoOptions(const QString& sudoBinary)
{
    QStringList options = {
        QStringLiteral("-u"),
        QStringLiteral("root")
    };
    if (sudoBinary.endsWith(QLatin1String("/kdesu"))) {
        // enable command line output
        options.append(QStringLiteral("-t"));
        // make the dialog transient for the current window
        options.append(QStringLiteral("--attach"));
        options.append(QString::number(KWindowSystem::activeWindow()));
    }
    return options;
}

void PerfRecord::startRecording(const QStringList &perfOptions, const QString &outputPath, bool recordAsSudo,
                                const QStringList &recordOptions, const QString &workingDirectory)
{
    // Reset perf record process to avoid getting signals from old processes
    if (m_perfRecordProcess) {
        m_perfRecordProcess->kill();
        m_perfRecordProcess->deleteLater();
    }
    m_perfRecordProcess = new QProcess(this);
    m_perfRecordProcess->setProcessChannelMode(QProcess::MergedChannels);

    QFileInfo outputFileInfo(outputPath);
    QString folderPath = outputFileInfo.dir().path();
    QFileInfo folderInfo(folderPath);
    if (!folderInfo.exists()) {
        emit recordingFailed(tr("Folder '%1' does not exist.").arg(folderPath));
        return;
    }
    if (!folderInfo.isDir()) {
        emit recordingFailed(tr("'%1' is not a folder.").arg(folderPath));
        return;
    }
    if (!folderInfo.isWritable()) {
        emit recordingFailed(tr("Folder '%1' is not writable.").arg(folderPath));
        return;
    }

    connect(m_perfRecordProcess, static_cast<void (QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished),
            this, [this] (int exitCode, QProcess::ExitStatus exitStatus) {
                Q_UNUSED(exitStatus)

                QFileInfo outputFileInfo(m_outputPath);
                if ((exitCode == EXIT_SUCCESS || (exitCode == SIGTERM && m_userTerminated)
                     || outputFileInfo.size() > 0) && outputFileInfo.exists()) {
                    if (!ensureFileReadable(m_outputPath)) {
                        emit recordingFailed(tr("Unable to make data file readable."));
                    } else {
                        emit recordingFinished(m_outputPath);
                    }
                } else {
                    emit recordingFailed(tr("Failed to record perf data, error code %1.").arg(exitCode));
                }
                m_userTerminated = false;
            });

    connect(m_perfRecordProcess, &QProcess::errorOccurred,
            this, [this] (QProcess::ProcessError error) {
                Q_UNUSED(error)
                if (!m_userTerminated) {
                    emit recordingFailed(m_perfRecordProcess->errorString());
                }
            });

    connect(m_perfRecordProcess, &QProcess::readyRead,
            this, [this] () {
                QString output = QString::fromUtf8(m_perfRecordProcess->readAll());
                emit recordingOutput(output);
            });

    m_outputPath = outputPath;
    auto perfBinary = QStringLiteral("perf");

    if (!workingDirectory.isEmpty()) {
        m_perfRecordProcess->setWorkingDirectory(workingDirectory);
    }

    if (recordAsSudo) {
        // launch perf as root
        auto sudoBinary = sudoUtil();
        auto options = sudoOptions(sudoBinary);

        // perf and options
        options += {
            QStringLiteral("--"),
            perfBinary,
            QStringLiteral("record"),
            QStringLiteral("-o"),
            m_outputPath
        };
        options += perfOptions;

        // use runuser to launch command as original user
        options += QStringList {
            QStringLiteral("--"),
            QStringLiteral("runuser"),
            QStringLiteral("-u"),
            currentUsername(),
            QStringLiteral("--"),
        };

        // finally the actual client application and arguments
        options += recordOptions;

        emit recordingStarted(sudoBinary, options);
        m_perfRecordProcess->start(sudoBinary, options);
    } else {
        QStringList perfCommand =  {
            QStringLiteral("record"),
            QStringLiteral("-o"),
            m_outputPath
        };
        perfCommand += perfOptions;
        perfCommand += recordOptions;

        emit recordingStarted(perfBinary, perfCommand);
        m_perfRecordProcess->start(perfBinary, perfCommand);
    }
}

void PerfRecord::record(const QStringList &perfOptions, const QString &outputPath, bool recordAsSudo, const QStringList &pids)
{
    if (pids.empty()) {
        emit recordingFailed(tr("Process does not exist."));
        return;
    }

    QStringList recordOptions = { QStringLiteral("--pid"), pids.join(QLatin1Char(',')) };

    startRecording(perfOptions, outputPath, recordAsSudo, recordOptions);
}

void PerfRecord::record(const QStringList &perfOptions, const QString &outputPath, bool recordAsSudo,
                        const QString &exePath, const QStringList &exeOptions, const QString &workingDirectory)
{
    QFileInfo exeFileInfo(exePath);

    if (!exeFileInfo.exists()) {
        exeFileInfo.setFile(QStandardPaths::findExecutable(exePath));
    }

    if (!exeFileInfo.exists()) {
        emit recordingFailed(tr("File '%1' does not exist.").arg(exePath));
        return;
    }
    if (!exeFileInfo.isFile()) {
        emit recordingFailed(tr("'%1' is not a file.").arg(exePath));
        return;
    }
    if (!exeFileInfo.isExecutable()) {
        emit recordingFailed(tr("File '%1' is not executable.").arg(exePath));
        return;
    }

    QStringList recordOptions =  { exeFileInfo.absoluteFilePath() };
    recordOptions += exeOptions;

    startRecording(perfOptions, outputPath, recordAsSudo, recordOptions, workingDirectory);
}

const QString PerfRecord::perfCommand()
{
    if (m_perfRecordProcess) {
        return QStringLiteral("perf ") + m_perfRecordProcess->arguments().join(QLatin1Char(' '));
    } else {
        return {};
    }
}

void PerfRecord::stopRecording()
{
    if (m_perfRecordProcess) {
        m_userTerminated = true;
        m_perfRecordProcess->terminate();
    }
}

void PerfRecord::sendInput(const QByteArray& input)
{
    Q_ASSERT(m_perfRecordProcess);
    m_perfRecordProcess->write(input);
}

bool PerfRecord::ensureFileReadable(const QString &filePath)
{
    QFileInfo outputFile(filePath);
    QString username = currentUsername();

    if (!outputFile.isReadable()) {
        QString sudoExe = sudoUtil();
        if (sudoExe.isEmpty() || username.isEmpty()) {
            return false;
        }

        QStringList options = sudoOptions(sudoExe);
        options += QStringList{
            QStringLiteral("--"),
            QStringLiteral("chown"),
            username + QLatin1Char(':') + KUserGroup().name(),
            filePath
        };

        QProcess chownProcess;
        chownProcess.execute(sudoExe, options);
    }

    outputFile.refresh();
    return outputFile.isReadable();
}

QString PerfRecord::sudoUtil() const
{
    const auto commands = {
        QStringLiteral("kdesu"),
        QStringLiteral("gksu")
    };
    for (const auto& cmd : commands) {
        QString util = QStandardPaths::findExecutable(cmd);
        if (!util.isEmpty()) {
            return util;
        }
    }
    return {};
}

QString PerfRecord::currentUsername() const
{
    return KUser().loginName();
}
