#include "stdafx.h"
#include "EpgDBManager.h"

#include "../../Common/CommonDef.h"
#include "../../Common/TimeUtil.h"
#include "../../Common/StringUtil.h"
#include "../../Common/PathUtil.h"
#include "../../Common/EpgTimerUtil.h"
#include "../../Common/EpgDataCap3Util.h"
#include "../../Common/CtrlCmdUtil.h"

CEpgDBManager::CEpgDBManager()
{
	this->epgMapRefLock = std::make_pair(0, &this->epgMapLock);
	this->loadStop = false;
	this->loadForeground = false;
	this->initialLoadDone = false;
	this->archivePeriodSec = 0;
}

CEpgDBManager::~CEpgDBManager()
{
	CancelLoadData();
}

void CEpgDBManager::SetArchivePeriod(int periodSec)
{
	CBlockLock lock(&this->epgMapLock);
	//それほど長くない期間に制限する。より長期保存を許す場合は負荷に気をつけること
	this->archivePeriodSec = min(periodSec, 14 * 24 * 3600);
}

void CEpgDBManager::ReloadEpgData(bool foreground)
{
	CancelLoadData();

	//フォアグラウンド読み込みを中断した場合は引き継ぐ
	if( this->loadForeground == false ){
		this->loadForeground = foreground;
	}
	this->loadThread = thread_(LoadThread, this);
}

void CEpgDBManager::LoadThread(CEpgDBManager* sys)
{
	OutputDebugString(L"Start Load EpgData\r\n");
	DWORD time = GetTickCount();

	if( sys->loadForeground == false ){
		//バックグラウンドに移行
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	}
	CEpgDataCap3Util epgUtil;
	if( epgUtil.Initialize(FALSE) != NO_ERR ){
		OutputDebugString(L"★EpgDataCap3.dllの初期化に失敗しました。\r\n");
		sys->loadForeground = false;
		sys->initialLoadDone = true;
		return;
	}

	__int64 utcNow = GetNowI64Time() - I64_UTIL_TIMEZONE;

	//EPGファイルの検索
	vector<wstring> epgFileList;
	const fs_path settingPath = GetSettingPath();
	const fs_path epgDataPath = fs_path(settingPath).append(EPG_SAVE_FOLDER);

	WIN32_FIND_DATA findData;
	HANDLE find;

	//指定フォルダのファイル一覧取得
	find = FindFirstFile(fs_path(epgDataPath).append(L"*_epg.dat").c_str(), &findData);
	if( find != INVALID_HANDLE_VALUE ){
		do{
			__int64 fileTime = (__int64)findData.ftLastWriteTime.dwHighDateTime << 32 | findData.ftLastWriteTime.dwLowDateTime;
			if( (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 && fileTime != 0 ){
				//見つかったファイルを一覧に追加
				//名前順。ただしTSID==0xFFFFの場合は同じチャンネルの連続によりストリームがクリアされない可能性があるので後ろにまとめる
				WCHAR prefix = fileTime + 7*24*60*60*I64_1SEC < utcNow ? L'0' :
				               wcslen(findData.cFileName) < 12 || _wcsicmp(findData.cFileName + wcslen(findData.cFileName) - 12, L"ffff_epg.dat") ? L'1' : L'2';
				wstring item = prefix + fs_path(epgDataPath).append(findData.cFileName).native();
				epgFileList.insert(std::lower_bound(epgFileList.begin(), epgFileList.end(), item), item);
			}
		}while( FindNextFile(find, &findData) );
		FindClose(find);
	}

	DWORD loadElapsed = 0;
	DWORD loadTick = GetTickCount();

	//EPGファイルの解析
	for( vector<wstring>::iterator itr = epgFileList.begin(); itr != epgFileList.end(); itr++ ){
		if( sys->loadStop ){
			//キャンセルされた
			return;
		}
		//一時ファイルの状態を調べる。取得側のCreateFile(tmp)→CloseHandle(tmp)→CopyFile(tmp,master)→DeleteFile(tmp)の流れをある程度仮定
		wstring path = itr->c_str() + 1;
		HANDLE tmpFile = CreateFile((path + L".tmp").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
		DWORD tmpError = GetLastError();
		if( tmpFile != INVALID_HANDLE_VALUE ){
			tmpError = NO_ERROR;
			FILETIME ft;
			if( GetFileTime(tmpFile, NULL, NULL, &ft) == FALSE || ((LONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime) + 300*I64_1SEC < utcNow ){
				//おそらく後始末されていない一時ファイルなので無視
				tmpError = ERROR_FILE_NOT_FOUND;
			}
			CloseHandle(tmpFile);
		}
		if( (*itr)[0] == L'0' ){
			if( tmpError != NO_ERROR && tmpError != ERROR_SHARING_VIOLATION ){
				//1週間以上前かつ一時ファイルがないので削除
				DeleteFile(path.c_str());
				_OutputDebugString(L"★delete %s\r\n", path.c_str());
			}
		}else{
			BYTE readBuff[188*256];
			bool swapped = false;
			HANDLE file = INVALID_HANDLE_VALUE;
			if( tmpError == NO_ERROR ){
				//一時ファイルがあって書き込み中でない→コピー直前かもしれないので3秒待つ
				Sleep(3000);
			}else if( tmpError == ERROR_SHARING_VIOLATION ){
				//一時ファイルがあって書き込み中→もうすぐ上書きされるかもしれないのでできるだけ退避させる
				HANDLE masterFile = CreateFile(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
				if( masterFile != INVALID_HANDLE_VALUE ){
					file = CreateFile((path + L".swp").c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
					if( file != INVALID_HANDLE_VALUE ){
						swapped = true;
						DWORD read;
						while( ReadFile(masterFile, readBuff, sizeof(readBuff), &read, NULL) && read != 0 ){
							DWORD written;
							WriteFile(file, readBuff, read, &written, NULL);
						}
						SetFilePointer(file, 0, 0, FILE_BEGIN);
						tmpFile = CreateFile((path + L".tmp").c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
						if( tmpFile != INVALID_HANDLE_VALUE || GetLastError() != ERROR_SHARING_VIOLATION ){
							//退避中に書き込みが終わった
							if( tmpFile != INVALID_HANDLE_VALUE ){
								CloseHandle(tmpFile);
							}
							CloseHandle(file);
							file = INVALID_HANDLE_VALUE;
						}
					}
					CloseHandle(masterFile);
				}
				if( file == INVALID_HANDLE_VALUE ){
					Sleep(3000);
				}
			}
			if( file == INVALID_HANDLE_VALUE ){
				file = CreateFile(path.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
			}
			if( file == INVALID_HANDLE_VALUE ){
				_OutputDebugString(L"Error %s\r\n", path.c_str());
			}else{
				//PATを送る(ストリームを確実にリセットするため)
				DWORD seekPos = 0;
				DWORD read;
				for( DWORD i=0; ReadFile(file, readBuff, 188, &read, NULL) && read == 188; i+=188 ){
					//PID
					if( ((readBuff[1] & 0x1F) << 8 | readBuff[2]) == 0 ){
						//payload_unit_start_indicator
						if( (readBuff[1] & 0x40) != 0 ){
							if( seekPos != 0 ){
								break;
							}
						}else if( seekPos == 0 ){
							continue;
						}
						seekPos = i + 188;
						epgUtil.AddTSPacket(readBuff, 188);
					}
				}
				SetFilePointer(file, seekPos, 0, FILE_BEGIN);
				//TOTを先頭に持ってきて送る(ストリームの時刻を確定させるため)
				bool ignoreTOT = false;
				while( ReadFile(file, readBuff, 188, &read, NULL) && read == 188 ){
					if( ((readBuff[1] & 0x1F) << 8 | readBuff[2]) == 0x14 ){
						ignoreTOT = true;
						epgUtil.AddTSPacket(readBuff, 188);
						break;
					}
				}
				SetFilePointer(file, seekPos, 0, FILE_BEGIN);
				while( ReadFile(file, readBuff, sizeof(readBuff), &read, NULL) && read != 0 ){
					for( DWORD i=0; i<read; i+=188 ){
						if( ignoreTOT && ((readBuff[i+1] & 0x1F) << 8 | readBuff[i+2]) == 0x14 ){
							ignoreTOT = false;
						}else{
							epgUtil.AddTSPacket(readBuff+i, 188);
						}
					}
					if( sys->loadForeground == false ){
						//処理速度がだいたい2/3になるように休む。I/O負荷軽減が狙い
						DWORD tick = GetTickCount();
						loadElapsed += tick - loadTick;
						loadTick = tick;
						if( loadElapsed > 20 ){
							Sleep(min<DWORD>(loadElapsed / 2, 100));
							loadElapsed = 0;
							loadTick = GetTickCount();
						}
					}
				}
				CloseHandle(file);
			}
			if( swapped ){
				DeleteFile((path + L".swp").c_str());
			}
		}
	}

	//EPGデータを取得
	DWORD serviceListSize = 0;
	SERVICE_INFO* serviceList = NULL;
	if( epgUtil.GetServiceListEpgDB(&serviceListSize, &serviceList) == FALSE ){
		sys->loadForeground = false;
		sys->initialLoadDone = true;
		return;
	}
	map<LONGLONG, EPGDB_SERVICE_EVENT_INFO> nextMap;
	for( const SERVICE_INFO* info = serviceList; info != serviceList + serviceListSize; info++ ){
		LONGLONG key = Create64Key(info->original_network_id, info->transport_stream_id, info->service_id);
		EPGDB_SERVICE_EVENT_INFO itemZero = {};
		EPGDB_SERVICE_EVENT_INFO& item = nextMap.insert(std::make_pair(key, itemZero)).first->second;
		item.serviceInfo.ONID = info->original_network_id;
		item.serviceInfo.TSID = info->transport_stream_id;
		item.serviceInfo.SID = info->service_id;
		if( info->extInfo != NULL ){
			item.serviceInfo.service_type = info->extInfo->service_type;
			item.serviceInfo.partialReceptionFlag = info->extInfo->partialReceptionFlag;
			if( info->extInfo->service_provider_name != NULL ){
				item.serviceInfo.service_provider_name = info->extInfo->service_provider_name;
			}
			if( info->extInfo->service_name != NULL ){
				item.serviceInfo.service_name = info->extInfo->service_name;
			}
			if( info->extInfo->network_name != NULL ){
				item.serviceInfo.network_name = info->extInfo->network_name;
			}
			if( info->extInfo->ts_name != NULL ){
				item.serviceInfo.ts_name = info->extInfo->ts_name;
			}
			item.serviceInfo.remote_control_key_id = info->extInfo->remote_control_key_id;
		}
		epgUtil.EnumEpgInfoList(item.serviceInfo.ONID, item.serviceInfo.TSID, item.serviceInfo.SID, EnumEpgInfoListProc, &item);
	}
	epgUtil.UnInitialize();

	__int64 arcMax = GetNowI64Time();
	__int64 arcMin;
	{
		CBlockLock lock(&sys->epgMapLock);
		arcMin = sys->archivePeriodSec <= 0 ? LLONG_MAX : arcMax - sys->archivePeriodSec * I64_1SEC;
	}
	arcMax += 3600 * I64_1SEC;

	//初回はアーカイブファイルから読み込む
	map<LONGLONG, EPGDB_SERVICE_EVENT_INFO> arcFromFile;
	if( arcMin < LLONG_MAX && sys->epgArchive.empty() ){
		vector<BYTE> buff;
		std::unique_ptr<FILE, decltype(&fclose)> fp(secure_wfopen(fs_path(settingPath).append(L"EpgArc.dat").c_str(), L"rbN"), fclose);
		if( fp && _fseeki64(fp.get(), 0, SEEK_END) == 0 ){
			__int64 fileSize = _ftelli64(fp.get());
			if( 0 < fileSize && fileSize < INT_MAX ){
				buff.resize((size_t)fileSize);
				rewind(fp.get());
				if( fread(&buff.front(), 1, buff.size(), fp.get()) != buff.size() ){
					buff.clear();
				}
			}
		}
		if( buff.empty() == false ){
			WORD ver;
			DWORD readSize;
			vector<EPGDB_SERVICE_EVENT_INFO> list;
			if( ReadVALUE(&ver, &buff.front(), (DWORD)buff.size(), &readSize) &&
			    ReadVALUE2(ver, &list, &buff.front() + readSize, (DWORD)buff.size() - readSize, NULL) ){
				for( size_t i = 0; i < list.size(); i++ ){
					LONGLONG key = Create64Key(list[i].serviceInfo.ONID, list[i].serviceInfo.TSID, list[i].serviceInfo.SID);
					arcFromFile[key] = std::move(list[i]);
				}
			}
		}
	}

	if( sys->loadForeground == false ){
		//フォアグラウンドに復帰
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
	}
	for(;;){
		//データベースを排他する
		{
			CBlockLock lock(&sys->epgMapLock);
			if( sys->epgMapRefLock.first == 0 ){
				if( arcFromFile.empty() == false ){
					sys->epgArchive.swap(arcFromFile);
				}
				//アーカイブから古いイベントを消す
				for( auto itr = sys->epgArchive.begin(); itr != sys->epgArchive.end(); itr++ ){
					itr->second.eventList.erase(std::remove_if(itr->second.eventList.begin(), itr->second.eventList.end(), [=](const EPGDB_EVENT_INFO& a) {
						return ConvertI64Time(a.start_time) <= arcMin || ConvertI64Time(a.start_time) >= arcMax;
					}), itr->second.eventList.end());
				}
				//イベントをアーカイブに移動する
				for( auto itr = sys->epgMap.begin(); arcMin < LLONG_MAX && itr != sys->epgMap.end(); itr++ ){
					auto itrArc = sys->epgArchive.find(itr->first);
					if( itrArc != sys->epgArchive.end() ){
						itrArc->second.serviceInfo = std::move(itr->second.serviceInfo);
					}
					for( size_t i = 0; i < itr->second.eventList.size(); i++ ){
						if( itr->second.eventList[i].StartTimeFlag &&
						    itr->second.eventList[i].DurationFlag &&
						    ConvertI64Time(itr->second.eventList[i].start_time) < arcMax &&
						    ConvertI64Time(itr->second.eventList[i].start_time) > arcMin ){
							if( itrArc == sys->epgArchive.end() ){
								//サービスを追加
								itrArc = sys->epgArchive.insert(std::make_pair(itr->first, EPGDB_SERVICE_EVENT_INFO())).first;
								itrArc->second.serviceInfo = std::move(itr->second.serviceInfo);
							}
							itrArc->second.eventList.push_back(std::move(itr->second.eventList[i]));
						}
					}
				}

				//EPGデータを更新する
				sys->epgMap.swap(nextMap);

				//アーカイブから不要なイベントを消す
				for( auto itr = sys->epgMap.cbegin(); arcMin < LLONG_MAX && itr != sys->epgMap.end(); itr++ ){
					auto itrArc = sys->epgArchive.find(itr->first);
					if( itrArc != sys->epgArchive.end() ){
						//主データベースの最古より新しいものは不要
						__int64 minStart = LLONG_MAX;
						for( size_t i = 0; i < itr->second.eventList.size(); i++ ){
							if( itr->second.eventList[i].StartTimeFlag && ConvertI64Time(itr->second.eventList[i].start_time) < minStart ){
								minStart = ConvertI64Time(itr->second.eventList[i].start_time);
							}
						}
						itrArc->second.eventList.erase(std::remove_if(itrArc->second.eventList.begin(), itrArc->second.eventList.end(), [=](const EPGDB_EVENT_INFO& a) {
							return ConvertI64Time(a.start_time) + a.durationSec * I64_1SEC > minStart;
						}), itrArc->second.eventList.end());
					}
				}
				//アーカイブから空のサービスを消す
				for( auto itr = sys->epgArchive.begin(); itr != sys->epgArchive.end(); ){
					if( itr->second.eventList.empty() ){
						sys->epgArchive.erase(itr++);
					}else{
						itr++;
					}
				}
				break;
			}
		}
		Sleep(1);
	}
	if( sys->loadForeground == false ){
		//バックグラウンドに移行
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	}
	nextMap.clear();

	//アーカイブファイルに書き込む
	if( arcMin < LLONG_MAX ){
		vector<const EPGDB_SERVICE_EVENT_INFO*> valp;
		valp.reserve(sys->epgArchive.size());
		for( auto itr = sys->epgArchive.cbegin(); itr != sys->epgArchive.end(); valp.push_back(&(itr++)->second) );
		DWORD buffSize;
		std::unique_ptr<BYTE[]> buff = NewWriteVALUE2WithVersion(5, valp, buffSize);
		std::unique_ptr<FILE, decltype(&fclose)> fp(secure_wfopen(fs_path(settingPath).append(L"EpgArc.dat").c_str(), L"wbN"), fclose);
		if( fp ){
			fwrite(buff.get(), 1, buffSize, fp.get());
		}
	}

	_OutputDebugString(L"End Load EpgData %dmsec\r\n", GetTickCount()-time);

	sys->loadForeground = false;
	sys->initialLoadDone = true;
}

BOOL CALLBACK CEpgDBManager::EnumEpgInfoListProc(DWORD epgInfoListSize, EPG_EVENT_INFO* epgInfoList, LPVOID param)
{
	EPGDB_SERVICE_EVENT_INFO* item = (EPGDB_SERVICE_EVENT_INFO*)param;

	try{
		if( epgInfoList == NULL ){
			item->eventList.reserve(epgInfoListSize);
		}else{
			for( DWORD i=0; i<epgInfoListSize; i++ ){
				item->eventList.resize(item->eventList.size() + 1);
				ConvertEpgInfo(item->serviceInfo.ONID, item->serviceInfo.TSID, item->serviceInfo.SID, &epgInfoList[i], &item->eventList.back());
				if( item->eventList.back().hasShortInfo ){
					//ごく稀にAPR(改行)を含むため
					Replace(item->eventList.back().shortInfo.event_name, L"\r\n", L"");
				}
				//実装上は既ソートだが仕様ではないので挿入ソートしておく
				for( size_t j = item->eventList.size() - 1; j > 0 && item->eventList[j].event_id < item->eventList[j-1].event_id; j-- ){
					std::swap(item->eventList[j], item->eventList[j-1]);
				}
			}
		}
	}catch( std::bad_alloc& ){
		return FALSE;
	}
	return TRUE;
}

bool CEpgDBManager::IsLoadingData()
{
	return this->loadThread.joinable() && WaitForSingleObject(this->loadThread.native_handle(), 0) == WAIT_TIMEOUT;
}

void CEpgDBManager::CancelLoadData()
{
	if( this->loadThread.joinable() ){
		this->loadStop = true;
		this->loadThread.join();
		this->loadStop = false;
	}
}

void CEpgDBManager::SearchEpg(const EPGDB_SEARCH_KEY_INFO* keys, size_t keysSize, vector<SEARCH_RESULT_EVENT_DATA>* result) const
{
	SearchEpg(keys, keysSize, [=](vector<SEARCH_RESULT_EVENT>& val) {
		result->reserve(result->size() + val.size());
		for( vector<SEARCH_RESULT_EVENT>::iterator itr = val.begin(); itr != val.end(); itr++ ){
			result->resize(result->size() + 1);
			result->back().info = *itr->info;
			result->back().findKey.swap(itr->findKey);
		}
	});
}

void CEpgDBManager::SearchEvent(const EPGDB_SEARCH_KEY_INFO& key, vector<SEARCH_RESULT_EVENT>& result) const
{
	if( key.andKey.compare(0, 7, L"^!{999}") == 0 ){
		//無効を示すキーワードが指定されているので検索しない
		return ;
	}
	wstring andKey = key.andKey;
	bool caseFlag = false;
	if( andKey.compare(0, 7, L"C!{999}") == 0 ){
		//大小文字を区別するキーワードが指定されている
		andKey.erase(0, 7);
		caseFlag = true;
	}
	DWORD chkDurationMinSec = 0;
	DWORD chkDurationMaxSec = MAXDWORD;
	if( andKey.compare(0, 4, L"D!{1") == 0 ){
		LPWSTR endp;
		DWORD dur = wcstoul(andKey.c_str() + 3, &endp, 10);
		if( endp - andKey.c_str() == 12 && endp[0] == L'}' ){
			//番組長を絞り込むキーワードが指定されている
			andKey.erase(0, 13);
			chkDurationMinSec = dur / 10000 % 10000 * 60;
			chkDurationMaxSec = dur % 10000 == 0 ? MAXDWORD : dur % 10000 * 60;
		}
	}

	//キーワード分解
	vector<vector<pair<wstring, RegExpPtr>>> andKeyList;
	vector<pair<wstring, RegExpPtr>> notKeyList;

	if( key.regExpFlag ){
		//正規表現の単独キーワード
		if( andKey.empty() == false ){
			andKeyList.push_back(vector<pair<wstring, RegExpPtr>>());
			AddKeyword(andKeyList.back(), andKey, caseFlag, true, key.titleOnlyFlag != FALSE);
		}
		if( key.notKey.empty() == false ){
			AddKeyword(notKeyList, key.notKey, caseFlag, true, key.titleOnlyFlag != FALSE);
		}
	}else{
		//正規表現ではないのでキーワードの分解
		Replace(andKey, L"　", L" ");
		while( andKey.empty() == false ){
			wstring buff;
			Separate(andKey, L" ", buff, andKey);
			if( buff == L"|" ){
				//OR条件
				andKeyList.push_back(vector<pair<wstring, RegExpPtr>>());
			}else if( buff.empty() == false ){
				if( andKeyList.empty() ){
					andKeyList.push_back(vector<pair<wstring, RegExpPtr>>());
				}
				AddKeyword(andKeyList.back(), std::move(buff), caseFlag, false, key.titleOnlyFlag != FALSE);
			}
		}
		wstring notKey = key.notKey;
		Replace(notKey, L"　", L" ");
		while( notKey.empty() == false ){
			wstring buff;
			Separate(notKey, L" ", buff, notKey);
			if( buff.empty() == false ){
				AddKeyword(notKeyList, std::move(buff), caseFlag, false, key.titleOnlyFlag != FALSE);
			}
		}
	}

	size_t resultSize = result.size();
	auto compareResult = [](const SEARCH_RESULT_EVENT& a, const SEARCH_RESULT_EVENT& b) -> bool {
		return Create64PgKey(a.info->original_network_id, a.info->transport_stream_id, a.info->service_id, a.info->event_id) <
		       Create64PgKey(b.info->original_network_id, b.info->transport_stream_id, b.info->service_id, b.info->event_id);
	};
	wstring targetWord;
	vector<int> distForFind;
	
	//サービスごとに検索
	for( size_t i = 0; i < key.serviceList.size(); i++ ){
		auto itrService = this->epgMap.find(key.serviceList[i]);
		if( itrService != this->epgMap.end() ){
			//サービス発見
			for( auto itrEvent = itrService->second.eventList.cbegin(); itrEvent != itrService->second.eventList.end(); itrEvent++ ){
				if( key.freeCAFlag == 1 ){
					//無料放送のみ
					if( itrEvent->freeCAFlag != 0 ){
						//有料放送
						continue;
					}
				}else if( key.freeCAFlag == 2 ){
					//有料放送のみ
					if( itrEvent->freeCAFlag == 0 ){
						//無料放送
						continue;
					}
				}
				//ジャンル確認
				if( key.contentList.size() > 0 ){
					//ジャンル指定あるのでジャンルで絞り込み
					if( itrEvent->hasContentInfo == false ){
						if( itrEvent->hasShortInfo == false ){
							//2つめのサービス？対象外とする
							continue;
						}
						//ジャンル情報ない
						bool findNo = false;
						for( size_t j = 0; j < key.contentList.size(); j++ ){
							if( key.contentList[j].content_nibble_level_1 == 0xFF &&
							    key.contentList[j].content_nibble_level_2 == 0xFF ){
								//ジャンルなしの指定あり
								findNo = true;
								break;
							}
						}
						if( key.notContetFlag == 0 ){
							if( findNo == false ){
								continue;
							}
						}else{
							//NOT条件扱い
							if( findNo ){
								continue;
							}
						}
					}else{
						bool equal = IsEqualContent(key.contentList, itrEvent->contentInfo.nibbleList);
						if( key.notContetFlag == 0 ){
							if( equal == false ){
								//ジャンル違うので対象外
								continue;
							}
						}else{
							//NOT条件扱い
							if( equal ){
								continue;
							}
						}
					}
				}

				//映像確認
				if( key.videoList.size() > 0 ){
					if( itrEvent->hasComponentInfo == false ){
						continue;
					}
					WORD type = itrEvent->componentInfo.stream_content << 8 || itrEvent->componentInfo.component_type;
					if( std::find(key.videoList.begin(), key.videoList.end(), type) == key.videoList.end() ){
						continue;
					}
				}

				//音声確認
				if( key.audioList.size() > 0 ){
					if( itrEvent->hasAudioInfo == false ){
						continue;
					}
					bool findContent = false;
					for( size_t j=0; j<itrEvent->audioInfo.componentList.size(); j++ ){
						WORD type = itrEvent->audioInfo.componentList[j].stream_content << 8 | itrEvent->audioInfo.componentList[j].component_type;
						if( std::find(key.audioList.begin(), key.audioList.end(), type) != key.audioList.end() ){
							findContent = true;
							break;
						}
					}
					if( findContent == false ){
						continue;
					}
				}

				//時間確認
				if( key.dateList.size() > 0 ){
					if( itrEvent->StartTimeFlag == FALSE ){
						//開始時間不明なので対象外
						continue;
					}
					bool inTime = IsInDateTime(key.dateList, itrEvent->start_time);
					if( key.notDateFlag == 0 ){
						if( inTime == false ){
							//時間範囲外なので対象外
							continue;
						}
					}else{
						//NOT条件扱い
						if( inTime ){
							continue;
						}
					}
				}

				//番組長で絞り込み
				if( itrEvent->DurationFlag == FALSE ){
					//不明なので絞り込みされていれば対象外
					if( 0 < chkDurationMinSec || chkDurationMaxSec < MAXDWORD ){
						continue;
					}
				}else{
					if( itrEvent->durationSec < chkDurationMinSec || chkDurationMaxSec < itrEvent->durationSec ){
						continue;
					}
				}

				SEARCH_RESULT_EVENT addItem;
				addItem.info = &(*itrEvent);

				//キーワード確認
				if( itrEvent->hasShortInfo == false ){
					if( andKeyList.size() != 0 ){
						//内容にかかわらず対象外
						continue;
					}
				}
				if( FindKeyword(notKeyList, *itrEvent, targetWord, distForFind, caseFlag, false, false) ){
					//notキーワード見つかったので対象外
					continue;
				}
				if( andKeyList.empty() == false ){
					bool found = false;
					for( size_t j = 0; j < andKeyList.size(); j++ ){
						if( FindKeyword(andKeyList[j], *itrEvent, targetWord, distForFind, caseFlag, key.aimaiFlag != 0, true, &addItem.findKey) ){
							found = true;
							break;
						}
					}
					if( found == false ){
						//andキーワード見つからなかったので対象外
						continue;
					}
				}

				//resultSizeまで(既ソート)に存在しないときだけ追加
				vector<SEARCH_RESULT_EVENT>::iterator itrResult = std::lower_bound(result.begin(), result.begin() + resultSize, addItem, compareResult);
				if( itrResult == result.begin() + resultSize || compareResult(addItem, *itrResult) ){
					result.push_back(addItem);
				}

			}
		}
	}
	//全体をソートして重複削除
	std::sort(result.begin(), result.end(), compareResult);
	result.erase(std::unique(result.begin(), result.end(), [](const SEARCH_RESULT_EVENT& a, const SEARCH_RESULT_EVENT& b) {
		return a.info->original_network_id == b.info->original_network_id &&
		       a.info->transport_stream_id == b.info->transport_stream_id &&
		       a.info->service_id == b.info->service_id &&
		       a.info->event_id == b.info->event_id;
	}), result.end());
}

bool CEpgDBManager::IsEqualContent(const vector<EPGDB_CONTENT_DATA>& searchKey, const vector<EPGDB_CONTENT_DATA>& eventData)
{
	for( size_t i=0; i<searchKey.size(); i++ ){
		EPGDB_CONTENT_DATA c = searchKey[i];
		if( 0x60 <= c.content_nibble_level_1 && c.content_nibble_level_1 <= 0x7F ){
			//番組付属情報またはCS拡張用情報に変換する
			c.user_nibble_1 = c.content_nibble_level_1 & 0x0F;
			c.user_nibble_2 = c.content_nibble_level_2;
			c.content_nibble_level_2 = (c.content_nibble_level_1 - 0x60) >> 4;
			c.content_nibble_level_1 = 0x0E;
		}
		for( size_t j=0; j<eventData.size(); j++ ){
			if( c.content_nibble_level_1 == eventData[j].content_nibble_level_1 ){
				if( c.content_nibble_level_2 == 0xFF ){
					//中分類すべて
					return true;
				}
				if( c.content_nibble_level_2 == eventData[j].content_nibble_level_2 ){
					if( c.content_nibble_level_1 != 0x0E ){
						//拡張でない
						return true;
					}
					if( c.user_nibble_1 == eventData[j].user_nibble_1 ){
						if( c.user_nibble_2 == 0xFF ){
							//拡張中分類すべて
							return true;
						}
						if( c.user_nibble_2 == eventData[j].user_nibble_2 ){
							return true;
						}
					}
				}
			}
		}
	}
	return false;
}

bool CEpgDBManager::IsInDateTime(const vector<EPGDB_SEARCH_DATE_INFO>& dateList, const SYSTEMTIME& time)
{
	int weekMin = (time.wDayOfWeek * 24 + time.wHour) * 60 + time.wMinute;
	for( size_t i=0; i<dateList.size(); i++ ){
		int start = (dateList[i].startDayOfWeek * 24 + dateList[i].startHour) * 60 + dateList[i].startMin;
		int end = (dateList[i].endDayOfWeek * 24 + dateList[i].endHour) * 60 + dateList[i].endMin;
		if( start >= end ){
			if( start <= weekMin || weekMin <= end ){
				return true;
			}
		}else{
			if( start <= weekMin && weekMin <= end ){
				return true;
			}
		}
	}

	return false;
}

bool CEpgDBManager::FindKeyword(const vector<pair<wstring, RegExpPtr>>& keyList, const EPGDB_EVENT_INFO& info, wstring& word,
                                vector<int>& dist, bool caseFlag, bool aimai, bool andFlag, wstring* findKey)
{
	for( size_t i = 0; i < keyList.size(); i++ ){
		const wstring& key = keyList[i].first;
		if( i == 0 || key.compare(0, 7, keyList[i - 1].first) ){
			//検索対象が変わったので作成
			word.clear();
			if( key.compare(0, 7, L":title:") == 0 ){
				if( info.hasShortInfo ){
					word += info.shortInfo.event_name;
				}
			}else if( key.compare(0, 7, L":event:") == 0 ){
				if( info.hasShortInfo ){
					word += info.shortInfo.event_name;
					word += L"\r\n";
					word += info.shortInfo.text_char;
					if( info.hasExtInfo ){
						word += L"\r\n";
						word += info.extInfo.text_char;
					}
				}
			}else if( key.compare(0, 7, L":genre:") == 0 ){
				AppendEpgContentInfoText(word, info);
			}else if( key.compare(0, 7, L":video:") == 0 ){
				AppendEpgComponentInfoText(word, info);
			}else if( key.compare(0, 7, L":audio:") == 0 ){
				AppendEpgAudioComponentInfoText(word, info);
			}else{
				throw std::runtime_error("");
			}
			ConvertSearchText(word);
		}

		if( keyList[i].second ){
			//正規表現
			OleCharPtr target(SysAllocString(word.c_str()), SysFreeString);
			if( target ){
				IDispatch* pMatches;
				if( SUCCEEDED(keyList[i].second->Execute(target.get(), &pMatches)) ){
					std::unique_ptr<IMatchCollection, decltype(&ComRelease)> matches((IMatchCollection*)pMatches, ComRelease);
					long count;
					if( SUCCEEDED(matches->get_Count(&count)) && count > 0 ){
						if( andFlag == false ){
							//見つかったので終了
							return true;
						}
						if( findKey && i + 1 == keyList.size() ){
							//最終キーのマッチを記録
							IDispatch* pMatch;
							if( SUCCEEDED(matches->get_Item(0, &pMatch)) ){
								std::unique_ptr<IMatch2, decltype(&ComRelease)> match((IMatch2*)pMatch, ComRelease);
								BSTR value_;
								if( SUCCEEDED(match->get_Value(&value_)) ){
									OleCharPtr value(value_, SysFreeString);
									*findKey = SysStringLen(value.get()) ? value.get() : L"";
								}
							}
						}
					}else if( andFlag ){
						//見つからなかったので終了
						return false;
					}
				}else if( andFlag ){
					return false;
				}
			}else if( andFlag ){
				return false;
			}
		}else{
			//通常
			if( key.size() > 7 &&
			    (aimai ? FindLikeKeyword(key, 7, word, dist, caseFlag) :
			     caseFlag ? std::search(word.begin(), word.end(), key.begin() + 7, key.end()) != word.end() :
			                std::search(word.begin(), word.end(), key.begin() + 7, key.end(),
			                            [](wchar_t l, wchar_t r) { return (L'a' <= l && l <= L'z' ? l - L'a' + L'A' : l) ==
			                                                              (L'a' <= r && r <= L'z' ? r - L'a' + L'A' : r); }) != word.end()) ){
				if( andFlag == false ){
					//見つかったので終了
					return true;
				}
			}else if( andFlag ){
				//見つからなかったので終了
				return false;
			}
		}
	}

	if( andFlag && findKey ){
		//見つかったキーを記録
		size_t n = findKey->size();
		for( size_t i = 0; i < keyList.size(); i++ ){
			if( keyList[i].second == NULL ){
				if( n == 0 && findKey->empty() == false ){
					*findKey += L' ';
				}
				findKey->insert(findKey->size() - n, keyList[i].first, 7, wstring::npos);
				if( n != 0 ){
					findKey->insert(findKey->end() - n, L' ');
				}
			}
		}
	}
	return andFlag;
}

bool CEpgDBManager::FindLikeKeyword(const wstring& key, size_t keyPos, const wstring& word, vector<int>& dist, bool caseFlag)
{
	//編集距離がしきい値以下になる文字列が含まれるか調べる
	size_t l = 0;
	size_t curr = key.size() - keyPos + 1;
	dist.assign(curr * 2, 0);
	for( size_t i = 1; i < curr; i++ ){
		dist[i] = dist[i - 1] + 1;
	}
	for( size_t i = 0; i < word.size(); i++ ){
		wchar_t x = word[i];
		for( size_t j = 0; j < key.size() - keyPos; j++ ){
			wchar_t y = key[j + keyPos];
			if( caseFlag && x == y ||
			    caseFlag == false && (L'a' <= x && x <= L'z' ? x - L'a' + L'A' : x) == (L'a' <= y && y <= L'z' ? y - L'a' + L'A' : y) ){
				dist[curr + j + 1] = dist[l + j];
			}else{
				dist[curr + j + 1] = 1 + (dist[l + j] < dist[l + j + 1] ? min(dist[l + j], dist[curr + j]) : min(dist[l + j + 1], dist[curr + j]));
			}
		}
		//75%をしきい値とする
		if( dist[curr + key.size() - keyPos] * 4 <= (int)(key.size() - keyPos) ){
			return true;
		}
		std::swap(l, curr);
	}
	return false;
}

void CEpgDBManager::AddKeyword(vector<pair<wstring, RegExpPtr>>& keyList, wstring key, bool caseFlag, bool regExp, bool titleOnly)
{
	keyList.push_back(std::make_pair(wstring(), RegExpPtr(NULL, ComRelease)));
	if( regExp ){
		key = (titleOnly ? L"::title:" : L"::event:") + key;
	}
	size_t regPrefix = key.compare(0, 2, L"::") ? 0 : 1;
	if( key.compare(regPrefix, 7, L":title:") &&
	    key.compare(regPrefix, 7, L":event:") &&
	    key.compare(regPrefix, 7, L":genre:") &&
	    key.compare(regPrefix, 7, L":video:") &&
	    key.compare(regPrefix, 7, L":audio:") ){
		//検索対象が不明なので指定する
		key = (titleOnly ? L":title:" : L":event:") + key;
	}else if( regPrefix != 0 ){
		key.erase(0, 1);
		//旧い処理では対象を全角空白のまま比較していたため正規表現も全角のケースが多い。特別に置き換える
		Replace(key, L"　", L" ");
		//RegExpオブジェクトを構築しておく
		void* pv;
		if( SUCCEEDED(CoCreateInstance(CLSID_RegExp, NULL, CLSCTX_INPROC_SERVER, IID_IRegExp, &pv)) ){
			keyList.back().second.reset((IRegExp*)pv);
			OleCharPtr pattern(SysAllocString(key.c_str() + 7), SysFreeString);
			if( pattern &&
			    SUCCEEDED(keyList.back().second->put_IgnoreCase(caseFlag ? VARIANT_FALSE : VARIANT_TRUE)) &&
			    SUCCEEDED(keyList.back().second->put_Pattern(pattern.get())) ){
				keyList.back().first.swap(key);
				return;
			}
			keyList.back().second.reset();
		}
		//空(常に不一致)にする
		key.erase(7);
	}
	ConvertSearchText(key);
	keyList.back().first.swap(key);
}

bool CEpgDBManager::GetServiceList(vector<EPGDB_SERVICE_INFO>* list) const
{
	CRefLock lock(&this->epgMapRefLock);

	if( this->epgMap.empty() ){
		return false;
	}
	list->reserve(list->size() + this->epgMap.size());
	for( auto itr = this->epgMap.cbegin(); itr != this->epgMap.end(); itr++ ){
		list->push_back(itr->second.serviceInfo);
	}
	return true;
}

bool CEpgDBManager::SearchEpg(
	WORD ONID,
	WORD TSID,
	WORD SID,
	WORD EventID,
	EPGDB_EVENT_INFO* result
	) const
{
	CRefLock lock(&this->epgMapRefLock);

	LONGLONG key = Create64Key(ONID, TSID, SID);
	auto itr = this->epgMap.find(key);
	if( itr != this->epgMap.end() ){
		EPGDB_EVENT_INFO infoKey;
		infoKey.event_id = EventID;
		auto itrInfo = std::lower_bound(itr->second.eventList.begin(), itr->second.eventList.end(), infoKey,
		                                [](const EPGDB_EVENT_INFO& a, const EPGDB_EVENT_INFO& b) { return a.event_id < b.event_id; });
		if( itrInfo != itr->second.eventList.end() && itrInfo->event_id == EventID ){
			*result = *itrInfo;
			return true;
		}
	}
	return false;
}

bool CEpgDBManager::SearchEpg(
	WORD ONID,
	WORD TSID,
	WORD SID,
	LONGLONG startTime,
	DWORD durationSec,
	EPGDB_EVENT_INFO* result
	) const
{
	CRefLock lock(&this->epgMapRefLock);

	LONGLONG key = Create64Key(ONID, TSID, SID);
	auto itr = this->epgMap.find(key);
	if( itr != this->epgMap.end() ){
		for( auto itrInfo = itr->second.eventList.cbegin(); itrInfo != itr->second.eventList.end(); itrInfo++ ){
			if( itrInfo->StartTimeFlag != 0 && itrInfo->DurationFlag != 0 ){
				if( startTime == ConvertI64Time(itrInfo->start_time) &&
					durationSec == itrInfo->durationSec
					){
						*result = *itrInfo;
						return true;
				}
			}
		}
	}
	return false;
}

bool CEpgDBManager::SearchServiceName(
	WORD ONID,
	WORD TSID,
	WORD SID,
	wstring& serviceName
	) const
{
	CRefLock lock(&this->epgMapRefLock);

	LONGLONG key = Create64Key(ONID, TSID, SID);
	auto itr = this->epgMap.find(key);
	if( itr != this->epgMap.end() ){
		serviceName = itr->second.serviceInfo.service_name;
		return true;
	}
	return false;
}

//検索対象や検索パターンから全半角の区別を取り除く(旧ConvertText.txtに相当)
//ConvertText.txtと異なり半角濁点カナを(意図通り)置換する点、［］，．全角空白を置換する点、―(U+2015)ゐヰゑヱΖ(U+0396)を置換しない点に注意
void CEpgDBManager::ConvertSearchText(wstring& str)
{
	//全角英数およびこのテーブルにある文字列を置換する
	//最初の文字(UTF-16)をキーとしてソート済み。同一キー内の順序はマッチの優先順
	static const WCHAR convertFrom[][2] = {
		L"’", L"”", L"　",
		L"！", L"＃", L"＄", L"％", L"＆", L"（", L"）", L"＊", L"＋", L"，", L"\xFF0D", L"．", L"／",
		L"：", L"；", L"＜", L"＝", L"＞", L"？", L"＠", L"［", L"］", L"＾", L"＿", L"｀", L"｛", L"｜", L"｝", L"\xFF5E",
		L"｡", L"｢", L"｣", L"､", L"･", L"ｦ", L"ｧ", L"ｨ", L"ｩ", L"ｪ", L"ｫ", L"ｬ", L"ｭ", L"ｮ", L"ｯ", L"ｰ", L"ｱ", L"ｲ", L"ｳ", L"ｴ", L"ｵ",
		{L'ｶ', L'ﾞ'}, L"ｶ", {L'ｷ', L'ﾞ'}, L"ｷ", {L'ｸ', L'ﾞ'}, L"ｸ", {L'ｹ', L'ﾞ'}, L"ｹ", {L'ｺ', L'ﾞ'}, L"ｺ",
		{L'ｻ', L'ﾞ'}, L"ｻ", {L'ｼ', L'ﾞ'}, L"ｼ", {L'ｽ', L'ﾞ'}, L"ｽ", {L'ｾ', L'ﾞ'}, L"ｾ", {L'ｿ', L'ﾞ'}, L"ｿ",
		{L'ﾀ', L'ﾞ'}, L"ﾀ", {L'ﾁ', L'ﾞ'}, L"ﾁ", {L'ﾂ', L'ﾞ'}, L"ﾂ", {L'ﾃ', L'ﾞ'}, L"ﾃ", {L'ﾄ', L'ﾞ'}, L"ﾄ",
		L"ﾅ", L"ﾆ", L"ﾇ", L"ﾈ", L"ﾉ",
		{L'ﾊ', L'ﾞ'}, {L'ﾊ', L'ﾟ'}, L"ﾊ", {L'ﾋ', L'ﾞ'}, {L'ﾋ', L'ﾟ'}, L"ﾋ", {L'ﾌ', L'ﾞ'}, {L'ﾌ', L'ﾟ'}, L"ﾌ",
		{L'ﾍ', L'ﾞ'}, {L'ﾍ', L'ﾟ'}, L"ﾍ", {L'ﾎ', L'ﾞ'}, {L'ﾎ', L'ﾟ'}, L"ﾎ",
		L"ﾏ", L"ﾐ", L"ﾑ", L"ﾒ", L"ﾓ", L"ﾔ", L"ﾕ", L"ﾖ", L"ﾗ", L"ﾘ", L"ﾙ", L"ﾚ", L"ﾛ", L"ﾜ", L"ﾝ", L"ﾞ", L"ﾟ",
		L"￥",
	};
	static const WCHAR convertTo[] = {
		L'\'', L'"', L' ',
		L'!', L'#', L'$', L'%', L'&', L'(', L')', L'*', L'+', L',', L'-', L'.', L'/',
		L':', L';', L'<', L'=', L'>', L'?', L'@', L'[', L']', L'^', L'_', L'`', L'{', L'|', L'}', L'~',
		L'。', L'「', L'」', L'、', L'・', L'ヲ', L'ァ', L'ィ', L'ゥ', L'ェ', L'ォ', L'ャ', L'ュ', L'ョ', L'ッ', L'ー', L'ア', L'イ', L'ウ', L'エ', L'オ',
		L'ガ', L'カ', L'ギ', L'キ', L'グ', L'ク', L'ゲ', L'ケ', L'ゴ', L'コ',
		L'ザ', L'サ', L'ジ', L'シ', L'ズ', L'ス', L'ゼ', L'セ', L'ゾ', L'ソ',
		L'ダ', L'タ', L'ヂ', L'チ', L'ヅ', L'ツ', L'デ', L'テ', L'ド', L'ト',
		L'ナ', L'ニ', L'ヌ', L'ネ', L'ノ',
		L'バ', L'パ', L'ハ', L'ビ', L'ピ', L'ヒ', L'ブ', L'プ', L'フ',
		L'ベ', L'ペ', L'ヘ', L'ボ', L'ポ', L'ホ',
		L'マ', L'ミ', L'ム', L'メ', L'モ', L'ヤ', L'ユ', L'ヨ', L'ラ', L'リ', L'ル', L'レ', L'ロ', L'ワ', L'ン', L'゛', L'゜',
		L'\\',
	};

	for( wstring::iterator itr = str.begin(), itrEnd = str.end(); itr != itrEnd; itr++ ){
		//注意: これは符号位置の連続性を利用してテーブル参照を減らすための条件。上記のテーブルを弄る場合はここを確認すること
		WCHAR c = *itr;
		if( (L'！' <= c && c <= L'￥') || c == L'　' || c == L'’' || c == L'”' ){
			if( L'０' <= c && c <= L'９' ){
				*itr = c - L'０' + L'0';
			}else if( L'Ａ' <= c && c <= L'Ｚ' ){
				*itr = c - L'Ａ' + L'A';
			}else if( L'ａ' <= c && c <= L'ｚ' ){
				*itr = c - L'ａ' + L'a';
			}else{
				const WCHAR (*f)[2] = std::lower_bound(convertFrom, convertFrom + _countof(convertFrom), &*itr,
				                                       [](LPCWSTR a, LPCWSTR b) { return (unsigned short)a[0] < (unsigned short)b[0]; });
				for( ; f != convertFrom + _countof(convertFrom) && (*f)[0] == c; f++ ){
					if( (*f)[1] == L'\0' ){
						*itr = convertTo[f - convertFrom];
						break;
					}else if( itr + 1 != itrEnd && *(itr + 1) == (*f)[1] ){
						size_t i = itrEnd - itr - 1;
						str.replace(itr, itr + 2, 1, convertTo[f - convertFrom]);
						//イテレータを再有効化
						itrEnd = str.end();
						itr = itrEnd - i;
						break;
					}
				}
			}
		}
	}
}

