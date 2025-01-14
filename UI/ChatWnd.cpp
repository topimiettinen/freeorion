#include "ChatWnd.h"

#include "CUIControls.h"
#include "PlayerListWnd.h"
#include "../client/human/HumanClientApp.h"
#include "../Empire/Empire.h"
#include "../Empire/EmpireManager.h"
#include "../network/Message.h"
#include "../network/ClientNetworking.h"
#include "../universe/Universe.h"
#include "../universe/Ship.h"
#include "../universe/ShipDesign.h"
#include "../universe/System.h"
#include "../universe/Special.h"
#include "../universe/Species.h"
#include "../universe/Tech.h"
#include "../universe/Building.h"
#include "../util/Directories.h"
#include "../util/i18n.h"
#include "../util/Logger.h"
#include "../util/OptionsDB.h"

#include <GG/GUI.h>

#include <boost/algorithm/string/predicate.hpp>


// boost::spirit::classic pulls in windows.h which in turn defines the macros
// SendMessage. Undefining those should avoid name collisions with FreeOrion
// function names
#include <boost/spirit/include/classic.hpp>
#ifdef FREEORION_WIN32
#  undef SendMessage
#endif


namespace {
    const std::string OPEN_TAG = "[[";
    const std::string CLOSE_TAG = "]]";
    const std::string SEPARATOR = ",";

    /** Converts (first, last) to a string, looks it up as a key in the
      * stringtable, then then appends this to the end of the string str. */
    struct UserStringSubstituteAndAppend {
        UserStringSubstituteAndAppend(std::string& str) :
            m_str(str)
        {
            //std::cout << "UserStringSubstituteAndAppend with str:" << str << std::endl;
        }

        void operator()(const char* first, const char* last) const {
            std::string token(first, last);
            if (token.empty() || !UserStringExists(token))
                return;
            m_str += UserString(token);
            //std::cout << "str at end of UserStringSubstituteAndAppend operator():" << m_str << std::endl;
        }

        std::string& m_str;
    };

    // sticks a sequence of characters onto the end of a string
    struct StringAppend {
        StringAppend(std::string& str) :
            m_str(str)
        {
            //std::cout << "StringAppend with str:" << str << std::endl;
        }

        void operator()(const char* first, const char* last) const {
            m_str += std::string(first, last);
            //std::cout << "str at end of StringAppend operator():" << m_str << std::endl;
        }

        std::string& m_str;
    };

    // replaces \a formatter_ with a new formatter with the template string
    // from within the specified range of characters \a first to \a last.
    struct StartNewFormatterFromUserString {
        StartNewFormatterFromUserString(boost::format& formatter_) :
            formatter(formatter_)
        {}

        void operator()(const char* first, const char* last) const {
            std::string token(first, last);
            if (token.empty())
                formatter = FlexibleFormat("");
            else if (UserStringExists(token))
                formatter = FlexibleFormat(UserString(token));
            else
                formatter = FlexibleFormat(token);
            //std::cout << std::endl << "Started new formatter with token:" << token << std::endl;
        }

        boost::format& formatter;
    };

    // Each time called, binds the next argument to \a formatter_ with the
    // string within the specified range of characters \a first to \a last.
    struct BindFormatterArgument {
        BindFormatterArgument(boost::format& formatter_) :
            formatter(formatter_)
        {}

        void operator()(const char* first, const char* last) const {
            std::string token(first, last);
            //std::cout << "Binding formatter arg:" << arg_num << " with str:" << token << std::endl;
            formatter.bind_arg(arg_num++, token);
        }

        boost::format& formatter;
        mutable int arg_num = 1;
    };

    // Appends to \a str_ the string extracted from \a formatter. The
    // character range \a first to \a last is ignored.
    struct AppendFormatterString {
        AppendFormatterString(const boost::format& formatter_, std::string& str_) :
            formatter(formatter_),
            m_str(str_)
        {}

        void operator()(const char* first, const char* last) const {
            m_str += formatter.str();
            //std::cout << "Appending formatted str:" << formatter.str() << std::endl;
        }

        const boost::format& formatter;
        std::string& m_str;
    };

    // finds instances of stringtable substitutions and/or string formatting
    // within the text \a input and evaluates them. [[KEY]] will be looked up
    // in the stringtable, and if found, replaced with the corresponding
    // stringtable entry. If not found, KEY is used instead. [[KEY,var1,var2]]
    // will look up KEY in the stringtable or use just KEY if there is no
    // such stringtable entry, and then substitute var1 for all instances of %1%
    // in the string, and var2 for all instances of %2% in the string. Any
    // intance of %3% or higher numbers will be deleted from the string, unless
    // a third or more parameters are specified.
    std::string StringtableTextSubstitute(const std::string& input) {
        std::string retval;

        namespace classic = boost::spirit::classic;
        classic::rule<> token = *(classic::anychar_p - CLOSE_TAG.c_str() - SEPARATOR.c_str());

        classic::rule<> var = OPEN_TAG.c_str() >> token[UserStringSubstituteAndAppend(retval)] >> CLOSE_TAG.c_str();
        classic::rule<> non_var = classic::anychar_p - OPEN_TAG.c_str();

        boost::format formatter;
        classic::rule<> substitution = OPEN_TAG.c_str() >> token[StartNewFormatterFromUserString(formatter)]
                                     >> *(SEPARATOR.c_str() >> token[BindFormatterArgument(formatter)])
                                     >> CLOSE_TAG.c_str();


        // parse and substitute variables
        try {
            classic::parse(input.c_str(), *(
                  non_var[StringAppend(retval)]
                | substitution[AppendFormatterString(formatter, retval)]
                //| var
                )
            );
        } catch (const std::exception&) {
            ErrorLogger() << "StringtableTextSubstitute caught exception when parsing input: " << input;
        }

        return retval;
    }
}

class MessageWndEdit : public CUIEdit {
public:
    //! \name Structors //@{
    MessageWndEdit();
    //@}

    //! \name Mutators //@{
    void KeyPress(GG::Key key, std::uint32_t key_code_point, GG::Flags<GG::ModKey> mod_keys) override;
    bool AutoComplete() override;   //!< Autocomplete current word
    //@}

    /** emitted when user presses enter/return while entering text */
    mutable boost::signals2::signal<void ()> TextEnteredSignal;
    mutable boost::signals2::signal<void ()> UpPressedSignal;
    mutable boost::signals2::signal<void ()> DownPressedSignal;

private:
    void FindGameWords();                    //!< Finds all game words for autocomplete

    /** AutoComplete helper function */
    bool CompleteWord(const std::set<std::string>& names, const std::string& partial_word,
                      const std::pair<GG::CPSize, const GG::CPSize>& cursor_pos,
                      std::string& text);

    // Set for autocomplete game words
    std::set<std::string>       m_game_words;

    // Repeated autocomplete variables
     std::vector<std::string>   m_auto_complete_choices;
     unsigned int               m_repeated_tab_count = 0;
     std::string                m_last_line_read;
     std::string                m_last_game_word;
};

////////////////////
// MessageWndEdit //
////////////////////
MessageWndEdit::MessageWndEdit() :
    CUIEdit("")
{}

void MessageWndEdit::KeyPress(GG::Key key, std::uint32_t key_code_point,
                              GG::Flags<GG::ModKey> mod_keys)
{
    switch (key) {
    case GG::GGK_RETURN:
    case GG::GGK_KP_ENTER:
        TextEnteredSignal();
        break;
    case GG::GGK_UP:
        UpPressedSignal();
        break;
    case GG::GGK_DOWN:
        DownPressedSignal();
        break;
    default:
        break;
    }
    CUIEdit::KeyPress(key, key_code_point, mod_keys);
}

void MessageWndEdit::FindGameWords() {
     // add player and empire names
    for (auto& entry : Empires()) {
        m_game_words.insert(entry.second->Name());
        m_game_words.insert(entry.second->PlayerName());
    }
    // add system names
    for (auto& system : GetUniverse().Objects().FindObjects<System>()) {
        if (system->Name() != "")
            m_game_words.insert(system->Name());
    }
     // add ship names
    for (auto& ship : GetUniverse().Objects().FindObjects<Ship>()) {
        if (ship->Name() != "")
            m_game_words.insert(ship->Name());
    }
     // add ship design names

    for (const auto& design : GetPredefinedShipDesignManager().GetOrderedShipDesigns()) {
        if (!design->Name().empty())
            m_game_words.insert(UserString(design->Name()));
    }
     // add specials names
    for (const std::string& special_name : SpecialNames()) {
        if (special_name != "")
            m_game_words.insert(UserString(special_name));
    }
     // add species names
    for (const auto& entry : GetSpeciesManager()) {
        if (entry.second->Name() != "")
            m_game_words.insert(UserString(entry.second->Name()));
    }
     // add techs names
    for (const std::string& tech_name : GetTechManager().TechNames()) {
        if (tech_name != "")
            m_game_words.insert(UserString(tech_name));
    }
    // add building type names
    for (const auto& entry : GetBuildingTypeManager()) {
        if (entry.second->Name() != "")
            m_game_words.insert(UserString(entry.second->Name()));
    }
    // add ship hulls
    for (const auto& design : GetPredefinedShipDesignManager().GetOrderedShipDesigns()) {
        if (!design->Hull().empty())
            m_game_words.insert(UserString(design->Hull()));
    }
    // add ship parts
    for (const auto& design : GetPredefinedShipDesignManager().GetOrderedShipDesigns()) {
        for (const std::string& part_name : design->Parts()) {
            if (part_name != "")
                m_game_words.insert(UserString(part_name));
        }
    }
 }

bool MessageWndEdit::AutoComplete() {
    std::string full_line = this->Text();

    // Check for repeated tab
    // if current line is same as the last read line
    if (m_last_line_read != "" && boost::equals(full_line, m_last_line_read)) {
        if (m_repeated_tab_count >= m_auto_complete_choices.size())
            m_repeated_tab_count = 0;

        std::string next_word = m_auto_complete_choices.at(m_repeated_tab_count);

        if (next_word != "") {
            // Remove the old choice from the line
            // and replace it with the next choice
            full_line = full_line.substr(0, full_line.size() - (m_last_game_word.size() + 1));
            full_line.insert(full_line.size(), next_word + " ");
            this->SetText(full_line);
            GG::CPSize move_cursor_to = full_line.size() + GG::CP1;
            this->SelectRange(move_cursor_to, move_cursor_to);
            m_last_game_word = next_word;
            m_last_line_read = this->Text();
        }
        ++m_repeated_tab_count;

        return true;    // indicates to calling signal that a hotkey press was processed

    } else {
        bool exact_match = false;

        auto cursor_pos = this->CursorPosn();
        if (cursor_pos.first == cursor_pos.second && 0 < cursor_pos.first && cursor_pos.first <= full_line.size()) {
            auto word_start = full_line.substr(0, Value(cursor_pos.first)).find_last_of(" :");
            if (word_start == std::string::npos)
                word_start = 0;
            else
                ++word_start;
            std::string partial_word = full_line.substr(word_start, Value(cursor_pos.first - word_start));
            if (partial_word.empty())
                return true;    // indicates to calling signal that a hotkey press was processed

            // Find game words to try an autocomplete
            FindGameWords();

            // See if word is an exact match with a game word
            for (const std::string& word : m_game_words) {
                if (boost::iequals(word, partial_word)) { // if there's an exact match, just add a space
                    full_line.insert(Value(cursor_pos.first), " ");
                    this->SetText(full_line);
                    this->SelectRange(cursor_pos.first + 1, cursor_pos.first + 1);
                    exact_match = true;
                    break;
                }
            }
            // If not an exact match try to complete the word
            if (!exact_match)
                CompleteWord(m_game_words, partial_word, cursor_pos, full_line);
        }
    }

    return true;    // indicates to calling signal that a hotkey press was processed
}

bool MessageWndEdit::CompleteWord(const std::set<std::string>& names, const std::string& partial_word,
                                  const std::pair<GG::CPSize, const GG::CPSize>& cursor_pos,
                                  std::string& full_line)
{
    // clear repeated tab variables
    m_auto_complete_choices.clear();
    m_repeated_tab_count = 0;

    std::string game_word;

    // Check if the partial_word is contained in any game words
    for (const std::string& temp_game_word : names) {
        if (temp_game_word.size() >= partial_word.size()) {
            // Add all possible word choices for repeated tab
            std::string game_word_partial = temp_game_word.substr(0, partial_word.size());
            if (!game_word_partial.empty() && boost::iequals(game_word_partial, partial_word))
                m_auto_complete_choices.push_back(temp_game_word);
        }
    }

    if (m_auto_complete_choices.empty())
        return false;

    // Grab first autocomplete choice
    game_word = m_auto_complete_choices.at(m_repeated_tab_count++);
    m_last_game_word = game_word;

    // Remove the partial_word from the line
    // and replace it with the properly formated game word
    full_line = full_line.substr(0, full_line.size() - partial_word.size());
    full_line.insert(full_line.size(), game_word + " ");
    this->SetText(full_line);
    m_last_line_read = this->Text();
    GG::CPSize move_cursor_to = full_line.size() + GG::CP1;
    this->SelectRange(move_cursor_to, move_cursor_to);
    return true;
}

////////////////////
//   MessageWnd   //
////////////////////
MessageWnd::MessageWnd(GG::Flags<GG::WndFlag> flags, const std::string& config_name) :
    CUIWnd(UserString("MESSAGES_PANEL_TITLE"), flags, config_name),
    m_display(nullptr),
    m_edit(nullptr),
    m_display_show_time(0),
    m_history(),
    m_history_position()
{}

void MessageWnd::CompleteConstruction() {
    CUIWnd::CompleteConstruction();

    m_display = GG::Wnd::Create<CUIMultiEdit>("", GG::MULTI_WORDBREAK | GG::MULTI_READ_ONLY |
                                                  GG::MULTI_TERMINAL_STYLE | GG::MULTI_INTEGRAL_HEIGHT);
    AttachChild(m_display);
    m_display->SetMaxLinesOfHistory(8000);

    m_edit = GG::Wnd::Create<MessageWndEdit>();
    AttachChild(m_edit);

    m_edit->TextEnteredSignal.connect(
        boost::bind(&MessageWnd::MessageEntered, this));
    m_edit->UpPressedSignal.connect(
        boost::bind(&MessageWnd::MessageHistoryUpRequested, this));
    m_edit->DownPressedSignal.connect(
        boost::bind(&MessageWnd::MessageHistoryDownRequested, this));
    m_edit->GainingFocusSignal.connect(
        TypingSignal);
    m_edit->LosingFocusSignal.connect(
        DoneTypingSignal);

    m_history.push_front("");

    Empires().DiplomaticStatusChangedSignal.connect(
        boost::bind(&MessageWnd::HandleDiplomaticStatusChange, this, _1, _2));

    DoLayout();
    SaveDefaultedOptions();
}

void MessageWnd::DoLayout() {
    const GG::Y PAD(3);
    m_display->SizeMove(GG::Pt(GG::X0, GG::Y0),
                        GG::Pt(ClientWidth(), ClientHeight() - PAD - m_edit->MinUsableSize().y));
    m_edit->SizeMove(GG::Pt(GG::X0, ClientHeight() - m_edit->MinUsableSize().y),
                     GG::Pt(ClientWidth() - GG::X(CUIWnd::INNER_BORDER_ANGLE_OFFSET), ClientHeight()));
}

void MessageWnd::CloseClicked() {
    StopFlash();
    ClosingSignal();
}

void MessageWnd::LClick(const GG::Pt& pt, GG::Flags<GG::ModKey> mod_keys) {
    CUIWnd::LClick(pt, mod_keys);
    StopFlash();
}

void MessageWnd::LDrag(const GG::Pt& pt, const GG::Pt& move, GG::Flags<GG::ModKey> mod_keys) {
    CUIWnd::LDrag(pt, move, mod_keys);
    StopFlash();
}

void MessageWnd::SizeMove(const GG::Pt& ul, const GG::Pt& lr) {
    const GG::Pt old_size = Size();
    CUIWnd::SizeMove(ul, lr);
    if (old_size != Size())
        RequirePreRender();
}

void MessageWnd::PreRender() {
    GG::Wnd::PreRender();
    DoLayout();
}

void MessageWnd::HandlePlayerChatMessage(const std::string& text,
                                         const std::string& player_name,
                                         GG::Clr text_color,
                                         const boost::posix_time::ptime& timestamp,
                                         int recipient_player_id)
{
    std::string filtered_message = StringtableTextSubstitute(text);
    std::string wrapped_text = RgbaTag(text_color);
    const std::string&& formatted_timestamp = ClientUI::FormatTimestamp(timestamp);
    if (IsValidUTF8(formatted_timestamp))
        wrapped_text += formatted_timestamp;
    if (player_name.empty())
        wrapped_text += filtered_message + "</rgba>";
    else
        wrapped_text += player_name + ": " + filtered_message + "</rgba>";
    TraceLogger() << "HandlePlayerChatMessage sender: " << player_name
                  << "  sender colour rgba tag: " << RgbaTag(text_color)
                  << "  filtered message: " << filtered_message
                  << "  timestamp text: " << ClientUI::FormatTimestamp(timestamp)
                  << "  wrapped text: " << wrapped_text;

    *m_display += wrapped_text + "\n";
    m_display_show_time = GG::GUI::GetGUI()->Ticks();

    // if client empire is target of message, show message window
    const ClientApp* app = ClientApp::GetApp();
    if (!app) {
        ErrorLogger() << "MessageWnd::HandlePlayerChatMessage couldn't get client app!";
        return;
    }
    int client_empire_id = app->EmpireID();  
    if (recipient_player_id == client_empire_id) {
        Flash();
        Show();
    }
}

void MessageWnd::HandleTurnPhaseUpdate(Message::TurnProgressPhase phase_id, bool prefixed /*= false*/) {
    std::string phase_str;
    switch (phase_id) {
    case Message::FLEET_MOVEMENT:
        phase_str = UserString("TURN_PROGRESS_PHASE_FLEET_MOVEMENT");
        break;
    case Message::COMBAT:
        phase_str = UserString("TURN_PROGRESS_PHASE_COMBAT");
        break;
    case Message::EMPIRE_PRODUCTION:
        phase_str = UserString("TURN_PROGRESS_PHASE_EMPIRE_GROWTH");
        break;
    case Message::WAITING_FOR_PLAYERS:
        phase_str = UserString("TURN_PROGRESS_PHASE_WAITING");
        break;
    case Message::PROCESSING_ORDERS:
        phase_str = UserString("TURN_PROGRESS_PHASE_ORDERS");
        break;
    case Message::COLONIZE_AND_SCRAP:
        phase_str = UserString("TURN_PROGRESS_COLONIZE_AND_SCRAP");
        break;
    case Message::DOWNLOADING:
        phase_str = UserString("TURN_PROGRESS_PHASE_DOWNLOADING");
        break;
    case Message::LOADING_GAME:
        phase_str = UserString("TURN_PROGRESS_PHASE_LOADING_GAME");
        break;
    case Message::GENERATING_UNIVERSE:
        phase_str = UserString("TURN_PROGRESS_PHASE_GENERATING_UNIVERSE");
        break;
    case Message::STARTING_AIS:
        phase_str = UserString("TURN_PROGRESS_STARTING_AIS");
        break;
    default:
        ErrorLogger() << "MessageWnd::HandleTurnPhaseUpdate got unknown turn phase id";
        return;
        break;
    }

    if (prefixed)
        *m_display += boost::str(FlexibleFormat(UserString("PLAYING_GAME")) % phase_str) + "\n";
    else
        *m_display += phase_str + "\n";
    m_display_show_time = GG::GUI::GetGUI()->Ticks();
}

void MessageWnd::HandleGameStatusUpdate(const std::string& text) {
    *m_display += text;
    m_display_show_time = GG::GUI::GetGUI()->Ticks();
}

void MessageWnd::HandleLogMessage(const std::string& text) {
    *m_display += text;
    m_display_show_time = GG::GUI::GetGUI()->Ticks();
}

void MessageWnd::HandleDiplomaticStatusChange(int empire1_id, int empire2_id) {
    const ClientApp* app = ClientApp::GetApp();
    if (!app) {
        ErrorLogger() << "MessageWnd::HandleDiplomaticStatusChange couldn't get client app!";
        return;
    }

    int client_empire_id = app->EmpireID();
    DiplomaticStatus status = Empires().GetDiplomaticStatus(empire1_id, empire2_id);
    std::string text;

    const Empire* empire1 = GetEmpire(empire1_id);
    const Empire* empire2 = GetEmpire(empire2_id);

    std::string empire1_str = GG::RgbaTag(empire1->Color()) + empire1->Name() + "</rgba>";
    std::string empire2_str = GG::RgbaTag(empire2->Color()) + empire2->Name() + "</rgba>";

    switch (status) {
    case DIPLO_WAR:
        text = boost::str(FlexibleFormat(UserString("MESSAGES_WAR_DECLARATION"))
                   % empire1_str % empire2_str);
        break;
    case DIPLO_PEACE:
        text = boost::str(FlexibleFormat(UserString("MESSAGES_PEACE_TREATY"))
                   % empire1_str % empire2_str);
        break;
    case DIPLO_ALLIED:
        text = boost::str(FlexibleFormat(UserString("MESSAGES_ALLIANCE"))
                   % empire1_str % empire2_str);
        break;
    default:
        ErrorLogger() << "MessageWnd::HandleDiplomaticStatusChange: no valid diplomatic status found.";
    }

    *m_display += text + "\n";
    m_display_show_time = GG::GUI::GetGUI()->Ticks();

    // if client empire is target of diplomatic status change, show message window
    if (empire2_id == client_empire_id) {
        Flash();
        Show();
    }
}

void MessageWnd::Clear()
{ m_display->Clear(); }

void MessageWnd::OpenForInput() {
    GG::GUI::GetGUI()->SetFocusWnd(m_edit);
    m_display_show_time = GG::GUI::GetGUI()->Ticks();
}

namespace {
    void SendChatMessage(const std::string& text, std::set<int> recipients) {
        ClientNetworking& net = HumanClientApp::GetApp()->Networking();

        if (recipients.empty()) {
            net.SendMessage(PlayerChatMessage(text));
        } else {
            recipients.insert(HumanClientApp::GetApp()->PlayerID());   // ensure recipient sees own sent message
            for (int recipient_id : recipients)
                net.SendMessage(PlayerChatMessage(text, recipient_id));
        }
    }

    void HandleTextCommand(const std::string& text) {
        if (text.size() < 2)
            return;

        // extract command and parameters substrings
        std::string command, params;
        std::string::size_type space_pos = text.find_first_of(' ');
        command = boost::trim_copy(text.substr(1, space_pos));
        if (command.empty())
            return;
        if (space_pos != std::string::npos)
            params = boost::trim_copy(text.substr(space_pos, std::string::npos));

        ClientUI* client_ui = ClientUI::GetClientUI();
        if (!client_ui)
            return;

        // execute command matching understood syntax
        if (boost::iequals(command, "zoom") && !params.empty()) {
            client_ui->ZoomToObject(params) || client_ui->ZoomToContent(params, true);   // params came from chat, so will be localized, so should be reverse looked up to find internal name from human-readable name for zooming to content
        } else if (boost::iequals(command, "pedia")) {
            if (params.empty())
                client_ui->ZoomToEncyclopediaEntry(UserStringNop("ENC_INDEX"));
            else
                client_ui->ZoomToContent(params, true);
        }
    }
}

void MessageWnd::MessageEntered() {
    std::string trimmed_text = boost::trim_copy(m_edit->Text());
    if (trimmed_text.empty())
        return;

    m_display_show_time = GG::GUI::GetGUI()->Ticks();

    // update history
    if (m_history.size() == 1 || m_history[1] != trimmed_text) {
        m_history[0] = trimmed_text;
        m_history.push_front("");
    } else {
        m_history[0] = "";
    }
    while (12 < static_cast<int>(m_history.size()) + 1)
        m_history.pop_back();
    m_history_position = 0;

    // if message starts with / treat it as a command
    if (trimmed_text[0] == '/') {
        HandleTextCommand(trimmed_text);
    } else {
        // otherwise, treat message as chat and send to recipients
        std::set<int> recipients;
        if (PlayerListWnd* player_list_wnd = ClientUI::GetClientUI()->GetPlayerListWnd().get())
            recipients = player_list_wnd->SelectedPlayerIDs();
        SendChatMessage(trimmed_text, recipients);
    }

    m_edit->Clear();
    StopFlash();
}

void MessageWnd::MessageHistoryUpRequested() {
    if (m_history_position < static_cast<int>(m_history.size()) - 1) {
        m_history[m_history_position] = m_edit->Text();
        ++m_history_position;
        m_edit->SetText(m_history[m_history_position]);
        m_edit->SelectRange(m_edit->Length(), m_edit->Length());    // put cursor at end of historical input
    }
}

void MessageWnd::MessageHistoryDownRequested() {
    if (0 < m_history_position) {
        m_history[m_history_position] = m_edit->Text();
        --m_history_position;
        m_edit->SetText(m_history[m_history_position]);
        m_edit->SelectRange(m_edit->Length(), m_edit->Length());    // put cursor at end of historical input
    }
}

