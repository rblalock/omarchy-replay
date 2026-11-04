#include "meeting_index.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QJsonObject>
#include <QTemporaryDir>
#include <sqlite3.h>
#include <iostream>
#include <cstring>
#include <stdexcept>

namespace {
void require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
// Fail midway through optional schema creation on newly opened connections.
// This exercises SQLite rollback and retry instead of inspecting SQL strings.
int denyPartialMeetingSchema(sqlite3 *db,char **,const sqlite3_api_routines *) {
    return sqlite3_set_authorizer(db, [](void *,int action,const char *name,const char *,const char *,const char *) {
        return action==SQLITE_CREATE_TABLE && name && std::strcmp(name,"meeting_tombstones")==0 ? SQLITE_DENY : SQLITE_OK;
    },nullptr);
}
void write(const QString &path,const QByteArray &bytes){QFile f(path);require(f.open(QIODevice::WriteOnly),"write synthetic source");require(f.write(bytes)==bytes.size(),"complete fixture write");}
QString meeting(const QString &root,const QString &name,qint64 started,const QString &text,bool imported=false){
    const QString dir=QDir(root).filePath(name);require(QDir().mkpath(dir),"create synthetic meeting");
    QJsonObject m{{"app","Omarchy Meeting Recorder"},{"version",1},{"title",name},{"started_at",started},{"duration_secs",300}};
    if(imported)m["imported"]="synthetic.ogg";
    write(dir+"/note.meeting-recorder",QJsonDocument(m).toJson());write(dir+"/transcript.md",text.toUtf8());return dir;
}
void settle(replay::MeetingImporter &importer,qint64 now,qint64 cutoff=0){
    for(int pass=0;pass<2;++pass){int guard=0;while(importer.sync(now,cutoff).more)require(++guard<400,"bounded scan failed to finish");}
}
QString archive(const QString &root,const QString &name){
    replay::RecorderOptions opts;opts.directory=root+'/'+name;opts.resume=true;opts.archiveFirst=true;opts.deferredOcr=true;opts.minFreeBytes=0;
    replay::Recorder recorder(opts);recorder.finish();return opts.directory;
}
QJsonObject recall(const QString &binary,const QStringList &arguments,bool ok=true){
    QProcess process;process.start(binary,QStringList{"recall"}+arguments);
    require(process.waitForFinished(30000),"recall CLI timed out");
    require((process.exitStatus()==QProcess::NormalExit && process.exitCode()==0)==ok,"recall CLI exit status");
    if(!ok)return {};
    const auto document=QJsonDocument::fromJson(process.readAllStandardOutput());require(document.isObject(),"recall CLI JSON");
    return document.object();
}
qint64 scalar(const QString &path,const char *sql){
    sqlite3 *db=nullptr;require(sqlite3_open(QFile::encodeName(path+"/index.sqlite").constData(),&db)==SQLITE_OK,"fixture database");
    sqlite3_stmt *s=nullptr;require(sqlite3_prepare_v2(db,sql,-1,&s,nullptr)==SQLITE_OK,"fixture query");require(sqlite3_step(s)==SQLITE_ROW,"fixture scalar");
    const auto n=sqlite3_column_int64(s,0);sqlite3_finalize(s);sqlite3_close(db);return n;
}
}
int main(int argc,char **argv){
    QCoreApplication app(argc,argv);
    try{
        QTemporaryDir temp;require(temp.isValid(),"temporary directory");const auto history=archive(temp.path(),"history");const auto source=temp.path()+"/meetings";QDir().mkpath(source);
        require(replay::listMeetings(temp.path()+"/absent").results.isEmpty(),"absent archive read creates nothing");
        require(replay::listMeetings(history).results.isEmpty(),"disabled archive reads empty");
        require(scalar(history,"SELECT count(*) FROM sqlite_master WHERE name='meeting_records'")==0,"read-only access created meeting tables");
        const qint64 now=QDateTime::currentMSecsSinceEpoch()+10000;const qint64 stamp=(now-3600000)/1000;
        const auto firstDir=meeting(source,"Budget review",stamp,"# Budget review\n\n[00:01] You: Continue the invoice discussion.\n\n[00:08] Pat: Continuity matters for invoice approval.\n");
        meeting(source,"A different meeting",stamp+1,"[00:00] You: Garden plan at café.\n");
        replay::MeetingImporter importer(history,source);
        require(importer.sync(now,0).imported==0,"unstable first sight imported immediately");settle(importer,now);
        const auto search=replay::searchMeetings(history,"contin");require(search.totalMatches==1 && search.results[0].matchingPassages==2,"prefix search must group two matching lines into one meeting");
        const auto id=search.results[0].meeting.id;
        require(search.results[0].meeting.transcript.isEmpty(),"search copied entire transcript into page metadata");
        require(replay::searchMeetings(history,"\" OR *").totalMatches==0,"raw FTS syntax escaped into query");
        require(replay::searchMeetings(history,"Budget review").totalMatches==1,"title search missing");
        const auto titleOnly=replay::searchMeetings(history,"different");
        require(titleOnly.totalMatches==1 && titleOnly.results[0].matchingPassages==0,"title-only match invented a transcript passage");
        const auto accentSearch=replay::searchMeetings(history,"cafe");
        require(accentSearch.totalMatches==1 && accentSearch.results[0].matchingPassages==1,"diacritic FTS match lost transcript highlighting");
        const QString unicode=QString::fromUtf8("🎙 café café CAFÉ continuity");
        const auto unicodeRanges=replay::meetingMatchRanges(unicode,"cafe");
        require(unicodeRanges==QVector<QPair<int,int>>{{3,4},{8,5},{14,4}},"Unicode normalization changed original UTF-16 highlight offsets");
        require(replay::meetingMatchRanges(QString::fromUtf8("caféine"),"caf").size()==1,"accent-folded prefix search disagreed with FTS");
        require(replay::listMeetings(history,1,1).results.size()==1 && replay::listMeetings(history,1,1).totalMatches==2,"meeting pagination lost total");
        const auto full=replay::readMeeting(history,id);require(full && full->transcript.contains("Continuity") && full->timeKnown,"full record read");
        require(replay::meetingMatchRanges(full->transcript,"contin").size()==2,"matching ranges differ from grouped passages");
        require(replay::meetingTimeline(history).size()==2,"known meetings lack start markers");
        {
            // Recall: bounded passages instead of transcripts, and time windows
            // overlap [start, start + duration]; undated meetings never match one.
            replay::MeetingSearchOptions options;options.passageLimit=3;
            auto bounded=replay::searchMeetings(history,"contin",100,0,replay::SearchMode::PrefixLastToken,options);
            require(bounded.totalMatches==1 && bounded.results[0].meeting.transcript.isEmpty(),"recall search copied a transcript");
            require(bounded.results[0].passages.size()==2 && bounded.results[0].passages[1].contains("Continuity"),"recall passages missing");
            options.passageLimit=1;bounded=replay::searchMeetings(history,"contin",100,0,replay::SearchMode::PrefixLastToken,options);
            require(bounded.results[0].passages.size()==1,"passage limit ignored");
            options.sinceMs=(stamp+299)*1000;require(replay::searchMeetings(history,"contin",100,0,replay::SearchMode::PrefixLastToken,options).totalMatches==1,"window missed a meeting still in progress");
            options.sinceMs=(stamp+301)*1000;require(replay::searchMeetings(history,"contin",100,0,replay::SearchMode::PrefixLastToken,options).totalMatches==0,"window kept an ended meeting");
            options.sinceMs=0;options.untilMs=(stamp-1)*1000;require(replay::searchMeetings(history,"contin",100,0,replay::SearchMode::PrefixLastToken,options).totalMatches==0,"window kept a later meeting");
            {
                // One very long transcript line is clipped around its match.
                QTemporaryDir other;require(other.isValid(),"temporary directory");
                const auto longHistory=archive(other.path(),"history");const auto longSource=other.path()+"/meetings";QDir().mkpath(longSource);
                meeting(longSource,"Long line",stamp,"[00:01] "+QString("filler ").repeated(200)+"needle "+QString("tail ").repeated(200)+"\n");
                replay::MeetingImporter longImporter(longHistory,longSource);settle(longImporter,now);
                options={};options.passageLimit=3;
                const auto clipped=replay::searchMeetings(longHistory,"needle",100,0,replay::SearchMode::PrefixLastToken,options);
                require(clipped.totalMatches==1 && clipped.results[0].passages.size()==1,"long-line passage missing");
                const auto passage=clipped.results[0].passages[0];
                require(passage.contains("needle") && passage.size()<=replay::MeetingPassageChars+2 && passage.startsWith(QString::fromUtf8("…")) && passage.endsWith(QString::fromUtf8("…")),"long-line passage was not clipped around its match");
            }
            if(argc>1){
                const QString binary=QString::fromLocal8Bit(argv[1]);
                const auto found=recall(binary,{"contin","--dir",history,"--source","meetings"});
                const auto entries=found.value("meetings").toArray();
                require(found.value("meetings_total_matches").toInteger()==1 && entries.size()==1,"recall CLI meeting search");
                const auto entry=entries[0].toObject();
                require(!entry.contains("transcript") && entry.value("passages").toArray().size()==2,"recall CLI returned a transcript instead of passages");
                require(entry.value("started_at_ms").toInteger()==stamp*1000 && found.value("meetings_next_offset").isNull(),"recall CLI meeting fields");
                const auto past=recall(binary,{"contin","--dir",history,"--source","meetings","--offset","1"});
                require(past.value("meetings").toArray().isEmpty() && past.value("meetings_next_offset").isNull(),"recall CLI meeting paging did not terminate");
                const auto later=recall(binary,{"contin","--dir",history,"--source","meetings","--since",QString::number((stamp+301)*1000)});
                require(later.value("meetings_total_matches").toInteger()==0,"recall CLI ignored the meeting time window");
                const auto fetched=recall(binary,{"--dir",history,"--meeting-id",QString::number(id)});
                require(fetched.value("meeting").toObject().value("transcript").toString().contains("Continuity"),"recall CLI meeting fetch");
                recall(binary,{"--dir",history,"--meeting-id","999999"},false);
                recall(binary,{"contin","--dir",history,"--meeting-id",QString::number(id)},false);
                recall(binary,{"--dir",history,"--meeting-id",QString::number(id),"--id","1"},false);
            }
        }
        settle(importer,now);require(replay::listMeetings(history).totalMatches==2,"repeat imports duplicated records");
        const auto renamed=source+"/Renamed folder";require(QDir().rename(firstDir,renamed),"rename source");settle(importer,now);
        require(replay::readMeeting(history,id)->sourceDirectory==renamed,"renaming meeting lost stable id/source link");
        write(renamed+"/transcript.md","[00:01] Pat: Revised invoice\n");settle(importer,now);
        require(replay::searchMeetings(history,"revised").totalMatches==1 && replay::searchMeetings(history,"contin").totalMatches==0,"editing source left old searchable text");
        write(renamed+"/note.meeting-recorder","{\"title\":");settle(importer,now);
        require(replay::readMeeting(history,id).has_value(),"partial manifest erased last committed transcript");
        // Restore manifest without touching retained identity.
        QJsonObject restored{{"title","Budget review"},{"started_at",stamp},{"duration_secs",300}};
        write(renamed+"/note.meeting-recorder",QJsonDocument(restored).toJson());settle(importer,now);
        const auto unknown=meeting(source,"Imported call",stamp-100,"[00:01] You: Imported context\n",true);settle(importer,now);
        const auto imported=replay::searchMeetings(history,"Imported context").results;
        require(imported.size()==1 && !imported[0].meeting.timeKnown && imported[0].meeting.startedAtMs==0,"imported audio treated file time as actual meeting time");
        require(replay::meetingTimeline(history).size()==2,"unknown meeting gained guessed timeline anchor");
        replay::deleteHistoryRange(history,stamp*1000,stamp*1000+1);settle(importer,now);
        require(!replay::readMeeting(history,id) && replay::searchMeetings(history,"revised").totalMatches==0,"deleted imported history resurrected on rescan");
        require(QFile::exists(renamed+"/transcript.md"),"Replay deletion touched external transcript");
        replay::maintainHistory(history,now+1);settle(importer,now+100);
        require(replay::listMeetings(history).totalMatches==0,"retention did not expire known and first-import unknown dates");
        require(QFile::exists(unknown+"/transcript.md"),"retention removed external files");
        const auto secondHistory=archive(temp.path(),"other-history");const auto unsafe=temp.path()+"/unsafe";QDir().mkpath(unsafe);
        const auto outside=meeting(temp.path(),"outside",stamp,"[00:01] secret: should not appear\n");
        require(QFile::link(outside,unsafe+"/linked-folder"),"create source symlink fixture");
        const auto linked=meeting(unsafe,"linked-text",stamp,"replace me");QFile::remove(linked+"/transcript.md");require(QFile::link(outside+"/transcript.md",linked+"/transcript.md"),"create transcript symlink");
        meeting(unsafe,"empty",stamp,"");
        meeting(unsafe,"oversized",stamp,QString(600*1024,'x'));
        replay::MeetingImporter guarded(secondHistory,unsafe);settle(guarded,now);
        require(replay::listMeetings(secondHistory).totalMatches==0,"unsafe or oversized source was imported");
        const auto gone=meeting(unsafe,"remove me",stamp,"[00:01] Searchable removable text\n");settle(guarded,now);
        require(replay::listMeetings(secondHistory).totalMatches==1,"valid source failed after unsafe neighbours");
        const auto offline=temp.path()+"/offline";require(QDir().rename(unsafe,offline),"make source unavailable");
        require(!guarded.sync(now,0).sourceAvailable && replay::listMeetings(secondHistory).totalMatches==1,"unavailable source erased imported meetings");
        require(QDir().rename(offline,unsafe),"restore source");require(QDir(gone).removeRecursively(),"remove source meeting");settle(guarded,now);
        require(replay::listMeetings(secondHistory).totalMatches==0,"source deletion did not reconcile");
        bool canceled=true;replay::MeetingImporter stopped(secondHistory,unsafe,[&]{return canceled;});require(stopped.sync(now,0).canceled,"cancellation ignored");
        const auto beforeEnable=archive(temp.path(),"before-enable");
        replay::deleteHistoryRange(beforeEnable,stamp*1000,stamp*1000+1);
        require(scalar(beforeEnable,"SELECT count(*) FROM sqlite_master WHERE name='meeting_records'")==0,"pre-enable delete eagerly created optional FTS schema");
        meeting(unsafe,"deleted before enable",stamp,"[00:01] Never import deleted interval\n");
        replay::MeetingImporter late(beforeEnable,unsafe);settle(late,now);
        require(replay::listMeetings(beforeEnable).totalMatches==0,"enabling import resurrected an earlier explicit deleted interval");
        const auto atomic=archive(temp.path(),"atomic-schema");
        replay::deleteHistoryRange(atomic,now+1000,now+2000);
        const auto extension=reinterpret_cast<void(*)()>(denyPartialMeetingSchema);
        require(sqlite3_auto_extension(extension)==SQLITE_OK,"install schema failure fixture");
        bool schemaFailed=false;
        try { replay::MeetingImporter interrupted(atomic,unsafe);interrupted.sync(now,0); }
        catch(const std::exception &) { schemaFailed=true; }
        sqlite3_cancel_auto_extension(extension);
        require(schemaFailed,"schema failure fixture did not reach interrupted migration");
        require(scalar(atomic,"SELECT count(*) FROM sqlite_master WHERE name LIKE 'meeting_%' AND name<>'meeting_deleted_ranges'")==0,
                "interrupted schema leaked partially published meeting tables or triggers");
        require(scalar(atomic,"SELECT count(*) FROM meeting_deleted_ranges")==1,"schema rollback lost an existing deletion policy");
        replay::MeetingImporter retrySchema(atomic,unsafe);settle(retrySchema,now);
        require(replay::listMeetings(atomic).totalMatches==1,"failed optional migration could not recover on retry");
        const auto constrained=archive(temp.path(),"constrained");
        replay::MeetingImporter budget(constrained,unsafe,{},16*1024*1024,1ULL<<62);
        require(budget.sync(now,0).limited,"meeting importer ignored free-disk reserve");settle(budget,now);
        require(replay::listMeetings(constrained).totalMatches==0,"constrained archive published a transcript");
        const auto pinned=archive(temp.path(),"pinned");const auto replacement=archive(temp.path(),"replacement");
        replay::MeetingImporter identity(pinned,unsafe);
        require(QDir().rename(pinned,pinned+"-old") && QDir().rename(replacement,pinned),"replace archive fixture");
        bool refused=false;try{identity.sync(now,0);}catch(const std::exception &){refused=true;}
        require(refused && scalar(pinned,"SELECT count(*) FROM sqlite_master WHERE name='meeting_records'")==0,"importer wrote into a replacement archive");
        const auto screens=archive(temp.path(),"screens");
        replay::RecorderOptions opts;opts.directory=screens;opts.resume=true;opts.archiveFirst=true;opts.deferredOcr=true;opts.minFreeBytes=0;
        replay::Recorder screenRecorder(opts);QImage image(32,32,QImage::Format_RGBA8888);image.fill(Qt::white);
        screenRecorder.addFrame(image,stamp*1000);screenRecorder.finish();
        require(replay::screenNearMeetingStart(screens,stamp*1000+5000).has_value(),"nearby retained screen unavailable");
        require(!replay::screenNearMeetingStart(screens,stamp*1000+11000),"unrelated distant screen offered as meeting context");
        replay::recordGap(screens,stamp*1000+1000,stamp*1000+6000,"synthetic excluded app");
        require(!replay::screenNearMeetingStart(screens,stamp*1000+5000),"meeting browsing crossed a capture exclusion gap");
        std::cout<<"meeting import, grouped search, lifecycle, retention and source safety passed\n";
        return 0;
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}
