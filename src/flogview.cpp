
#include "detail/flogview.h"

FLogViewBase::FLogViewBase(finalcut::FWidget* parent, uint_fast16_t logSize) : finalcut::FDialog{parent}, _logSize{logSize} {
    //
    // -widget config-
    //
    _textView.addCallback("mouse-wheel-up", this, &FLogViewBase::_loggerScrollUpCb);
    //_textView.ignorePadding();

    _toggleAutoScroll.setChecked(true);
    _toggleAutoScroll.addCallback("toggled", this, &FLogViewBase::_autoScrollToggleCb);

    _buttonPlay.addCallback("clicked", this, &FLogViewBase::_playButtonCb);
    _buttonPlay.setForegroundColor(finalcut::FColor::White);
    _buttonPlay.setFocusForegroundColor(finalcut::FColor::White);
    _buttonPlay.setFocus();

    _buttonClear.setForegroundColor(finalcut::FColor::White);
    _buttonClear.setFocusForegroundColor(finalcut::FColor::White);

    _lineEditFilter.setLabelText(L"Filter");
    _lineEditFilter.setLabelOrientation(finalcut::FLineEdit::LabelOrientation::Above);
    _lineEditFilter.unsetShadow();

    //
    // -static layout-
    //
    // play/pause button
    _buttonPlay.setGeometry(finalcut::FPoint{1,2}, finalcut::FSize{8, 1});

    // play/pause indicator
    _labelPlay.setGeometry(finalcut::FPoint{11,2}, finalcut::FSize{3, 1});

    // clear button
    _buttonClear.setGeometry(finalcut::FPoint{14,2}, finalcut::FSize{7, 1});

    // filter box
    _lineEditFilter.setGeometry(finalcut::FPoint{23,2}, finalcut::FSize{16, 3});
}

//////////////////////////////////////////////////////////////////////
//                                                                  //
//////////////////////////////////////////////////////////////////////

void FLogViewBase::clear(void) {
    _textView.clear();
}

void FLogViewBase::registerOnQuit(std::function<void(void)> cb) {
    _quitCb = cb;
}

//////////////////////////////////////////////////////////////////////
//                                                                  //
//////////////////////////////////////////////////////////////////////

void FLogViewBase::initLayout(void) {
    _adjust();
    finalcut::FDialog::initLayout();
}

void FLogViewBase::adjustSize(void) {
    _adjust();
    finalcut::FDialog::adjustSize();
}

void FLogViewBase::onClose(finalcut::FCloseEvent* event) {
    _quitCb();
    event->accept();
}

void FLogViewBase::onKeyPress (finalcut::FKeyEvent* event) {
    if(event->key() == finalcut::FKey::Ctrl_c) {
        _quitCb();
        event->accept();
    } else {
        finalcut::FDialog::onKeyPress(event);
    }
}

//////////////////////////////////////////////////////////////////////
//                                                                  //
//////////////////////////////////////////////////////////////////////

void FLogViewBase::_adjust(void) {
    // auto scroll toggle
    finalcut::FPoint togglePosition{static_cast<int>(getClientWidth()) - 14, 1};
    finalcut::FSize toggleSize{15, 3};
    _toggleGroup.setGeometry(togglePosition, toggleSize);
    _toggleAutoScroll.setGeometry(finalcut::FPoint{2,1}, finalcut::FSize{10, 1});

    // logger view
    finalcut::FPoint loggerPosition{1,4};
    finalcut::FSize loggerSize{getWidth(), getClientHeight() - 3};
    _textView.setGeometry(loggerPosition, loggerSize);
}

void FLogViewBase::_printLog(const std::wstring& logLine, LogLevel logLevel, std::string::size_type hglPos){
    finalcut::FString logLineFstring{logLine};
    _textView.append(logLineFstring);

    finalcut::FColor bg = finalcut::FColor::Blue;
    switch (logLevel) {
    case LogLevel::LOG_TRACE_L3:
    case LogLevel::LOG_TRACE_L2:
    case LogLevel::LOG_TRACE_L1:
    case LogLevel::LOG_DEBUG:
        bg = finalcut::FColor::LightGray;
        break;
    case LogLevel::LOG_INFO:
    case LogLevel::LOG_NOTICE:
        bg = finalcut::FColor::Blue;
        break;
    case LogLevel::LOG_WARNING:
        bg = finalcut::FColor::DarkOrange;
        break;
    case LogLevel::LOG_ERROR:
    case LogLevel::LOG_CRITICAL:
        bg = finalcut::FColor::Red;
        break;
    default:
        bg = finalcut::FColor::Purple;
        break;
    }
    if (!logLine.empty()) {
        _textView.addHighlight(_textView.getLines().size()-1,
                               finalcut::FTextView::FTextHighlight{0, logLine.size(), finalcut::FColorPair{finalcut::FColor::White, bg}});
    }

    if(hglPos != std::string::npos){
        _textView.addHighlight(_textView.getLines().size()-1, finalcut::FTextView::FTextHighlight{hglPos, _searchString.length(), finalcut::FColorPair{finalcut::FColor::Black, finalcut::FColor::Yellow}});
    }
}

void FLogViewBase::_adjustView(bool isPrinted, bool isShifted) {
    if(isPrinted){
        if(_autoScroll){
            _textView.scrollToEnd();
        } else {
            // scroll up by 1 to keep the same view when a new log is added and oldest one removed
            if(isShifted && (_textView.getScrollPos().getY() > 0)) {
                _textView.scrollBy(0, -1);
            }
        }
        _textView.redraw();
    } else {
        if(isShifted){
            _textView.redraw();
        }
    }
}

//////////////////////////////////////////////////////////////////////
//                                                                  //
//////////////////////////////////////////////////////////////////////

void FLogViewBase::_autoScrollToggleCb(void) {
    _autoScroll = _toggleAutoScroll.isChecked();
}

void FLogViewBase::_loggerScrollUpCb(void) {
    if(_autoScroll){
        _autoScroll = false;
        _toggleAutoScroll.setChecked(false);
        _toggleAutoScroll.redraw();
    }
}

void FLogViewBase::_playButtonCb(void){
    if(_isPlaying){
        _isPlaying = false;
        _labelPlay.setText(finalcut::FString{std::wstring{L"\U0001F534"}});
    } else {
        _isPlaying = true;
        _labelPlay.setText(finalcut::FString{std::wstring{L"\U0001F7E2"}});
    }
    _labelPlay.redraw();
}

void FLogViewBase::_filter(const LogList& logList) {
    bool clearFlag{false};
    
    _searchString = _lineEditFilter.getText().toWString();
    if(_searchString == L" "){
        _searchString.clear();
        clearFlag = true;
    }
    
    // clear log view
    _textView.clear();

    // search main list and print matching logs
    for(auto it = logList.cbegin(); it != logList.cend(); it++){
        if(clearFlag){
            _printLog(it->logString, it->logLevel);
        } else {
            std::string::size_type pos = it->logString.find(_searchString);
            if(pos != std::string::npos){
                _printLog(it->logString, it->logLevel, pos);
            }
        }
    }

    // scroll if needed and redraw logger view
    _adjustView(true, false);
}

void FLogViewBase::_log(LogList& logList, std::wstring&& logLine, LogLevel logLevel, bool isActive) {
    bool isShifted {false};
    bool isPrinted {false};

    // rollover if log display size is reached
    if(logList.size() >= _logSize) {
        if(isActive){
            //remove oldest log from display
            if(_searchString.empty()){
                // if search is not active than remove oldest log
                _textView.deleteLine(0);
                isShifted = true;
            } else {
                // if search is active than remove oldest log only if it matches the search string
                const std::wstring& oldestLog = logList.front().logString;
                if(oldestLog.find(_searchString) != std::wstring::npos){
                    _textView.deleteLine(0);
                    isShifted = true;
                }
            }
        }

        logList.pop_front();
    }

    // add log to display if necessary
    if(isActive) {
        if(_searchString.empty()) {
            // search is not active, print log
            _printLog(logLine, logLevel);
            isPrinted = true;
        } else {
            // search is active, print if log matches the search
            std::string::size_type pos = logLine.find(_searchString);
            if(pos != std::string::npos){
                _printLog(logLine, logLevel, pos);
                isPrinted = true;
            } 
        }

        // adjust text view component
        _adjustView(isPrinted, isShifted);
    }

    // add log to main list
    logList.emplace_back(LogItem{logLevel, std::move(logLine)});
}

void FLogViewBase::_printWithSearch(const LogItem& logItem) {
    bool emptyFlag = _searchString.empty();
    
    if(emptyFlag) {
        _printLog(logItem.logString, logItem.logLevel);
    } else {
        std::string::size_type pos = logItem.logString.find(_searchString);
        if(pos != std::string::npos){
            _printLog(logItem.logString, logItem.logLevel, pos);
        }
    }
}

//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////
//     ***FLogView (single view)**                                  //
//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////

FLogView::FLogView(finalcut::FWidget* parent, uint_fast16_t scrollBackLimit) : FLogViewBase{parent, scrollBackLimit}
{
    _lineEditFilter.addCallback("changed", this, &FLogView::_filterChangedCb);

    _buttonClear.addCallback("clicked", this, &FLogView::clear);
}

void FLogView::clear(void) {
    // lock logger view access
    std::lock_guard<std::mutex> lg(_loggerViewMtx);
    _mainLogList.clear();
    //_currentLogSize = 0;

    // call base class clear
    FLogViewBase::clear();
}

void FLogView::log(std::wstring&& logLine, LogLevel logLevel) {
    if(_isPlaying == false){
        return;
    }

    // lock logger view access
    std::lock_guard<std::mutex> lg(_loggerViewMtx);

    _log(_mainLogList, std::move(logLine), logLevel, true);
}

void FLogView::_filterChangedCb(void) {
    // lock logger view access
    std::lock_guard<std::mutex> lg(_loggerViewMtx);
    
    // pass logList for filter
    _filter(_mainLogList);    
}

//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////
//     ***FLogViewMulti (multi view)**                                  //
//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////

FLogViewMulti::FLogViewMulti(finalcut::FWidget* parent, uint_fast16_t scrollBackLimit, uint_fast16_t viewLimit) : FLogViewBase{parent, scrollBackLimit}, _viewSize{viewLimit}
{
    // configure dd-list
    _dropdownViewSelect.setLabelOrientation (finalcut::FLineEdit::LabelOrientation::Above);
    _dropdownViewSelect.unsetShadow();
    _dropdownViewSelect.unsetEditable();
    _dropdownViewSelect.addCallback("row-changed", this, &FLogViewMulti::_dropdownChangedCb);
    _dropdownViewSelect.addCallback("changed", this, &FLogViewMulti::_dropdownChangedCb);

    _lineEditFilter.addCallback("changed", this, &FLogViewMulti::_filterChangedCb);

    _buttonClear.addCallback("clicked", this, &FLogViewMulti::clear);
}

bool FLogViewMulti::createView(uint_fast16_t viewId, const std::string& viewName) {
    // lock logger view access
    std::lock_guard<std::mutex> lg(_loggerViewMtx);
    
    if(_viewMap.size() >= _viewSize) {
        return false;
    }
    
    // create a new log-list with id
    auto [iter, result] = _viewMap.try_emplace(viewId, LogList{});
    
    // return false if emplace is not succesful
    if(result) {
        _dropdownViewSelect.insert(viewName, uint_fast16_t{viewId});    // insert needs a non-cost rvalue for storing data
        if(_viewMap.size() == 1){
            _activeViewId = viewId;
            _dropdownViewSelect.setCurrentItem(0);  // is this needed?
        }
        _dropdownViewSelect.redraw();
    } 
    return result;
}

void FLogViewMulti::removeView(uint_fast16_t viewId) {
    // lock logger view access
    std::unique_lock<std::mutex> lg(_loggerViewMtx);

    if(_viewMap.find(viewId) == _viewMap.cend()) {
        return;
    }

    // locate and remove view from ddlist
    _isRemoving = true; // to return from _dropdownChangedCb triggered by setCurrentItem()
    for(std::size_t i = 1; i <= _dropdownViewSelect.getCount(); i++){
        _dropdownViewSelect.setCurrentItem(i);
        auto data = _dropdownViewSelect.getItemData<uint_fast16_t>();
        if(data == viewId){
            // ilgili view'ı sil
            _viewMap.erase(viewId);

            // remove list item and redraw
            _dropdownViewSelect.remove(i);
            _dropdownViewSelect.redraw();

            // call cb manually
            _isRemoving = false;
            lg.unlock(); // unlock before removing item to avoid deadlock in _dropdownChangedCb
            _dropdownChangedCb();
            break;
        }
    }
}

void FLogViewMulti::setViewSelectText(const std::string& textStr) {
    _dropdownViewSelect.setLabelText(textStr);
}

void FLogViewMulti::log(std::wstring&& logLine, LogLevel logLevel, uint_fast16_t viewId) {
    if(_isPlaying == false){
        return;
    }

    // lock logger view access
    std::lock_guard<std::mutex> lg(_loggerViewMtx);

    // get desired view log-list
    auto it = _viewMap.find(viewId);
    if(it == _viewMap.cend()) {
        return;
    }
    LogList& logList = it->second;

    // get isActive info
    bool isActive = (viewId == _activeViewId) ? true : false;

    _log(logList, std::move(logLine), logLevel, isActive);
}

void FLogViewMulti::clear(void){
    if(_viewMap.size() != 0) {
        // lock logger view access
        std::lock_guard<std::mutex> lg(_loggerViewMtx);
        _viewMap.find(_activeViewId)->second.clear();

        // call base class clear
        FLogViewBase::clear();
    }
}

//////////////////////////////////////////////////////////////////////
//                                                                  //
//////////////////////////////////////////////////////////////////////

void FLogViewMulti::initLayout(void) {
    _adjust();
    FLogViewBase::initLayout();
}

void FLogViewMulti::adjustSize(void) {
    _adjust();
    FLogViewBase::adjustSize();
}

void FLogViewMulti::_adjust(void) {
    _dropdownViewSelect.setGeometry({static_cast<int>(getClientWidth()) - 30, 2}, {15, 2});
}

//////////////////////////////////////////////////////////////////////
//                                                                  //
//////////////////////////////////////////////////////////////////////

void FLogViewMulti::_filterChangedCb(void) {    
    // pass active logList for filter
    if(_viewMap.size() != 0){
        // lock logger view access
        std::lock_guard<std::mutex> lg(_loggerViewMtx); 

        _filter(_viewMap.find(_activeViewId)->second);
    }
}

void FLogViewMulti::_dropdownChangedCb(void) {
    if(_isRemoving){
        return;
    }

    // lock logger view access
    std::lock_guard<std::mutex> lg(_loggerViewMtx);
    
    if(_dropdownViewSelect.getCount() == 0) {
        // lock logger view access
        // std::lock_guard<std::mutex> lg(_loggerViewMtx); 
        // clear logger view
        FLogViewBase::clear();
    } else {
        // get current displayed id
        auto currentViewId = _dropdownViewSelect.getItemData<uint_fast16_t>();
        
        if(currentViewId != _activeViewId) {
            _activeViewId = currentViewId;
            // lock logger view access
            // std::lock_guard<std::mutex> lg(_loggerViewMtx); 

            _filter(_viewMap.find(_activeViewId)->second);
        }
    }
}
