#ifndef CUSTOMEDIT_H_
#define CUSTOMEDIT_H_

#include "NuppelVideoPlayer.h"
#include "programinfo.h"
#include "mythscreentype.h"

class MythUITextEdit;
class MythUIButton;
class MythUIButtonList;
class MythUIButtonListItem;

class MPUBLIC CustomEdit : public MythScreenType
{
    Q_OBJECT
  public:

    CustomEdit(MythScreenStack *parent, ProgramInfo *m_pginfo = NULL);
   ~CustomEdit(void);
  
   bool Create();
   void customEvent(QEvent *event);
 
  protected slots:
    void ruleChanged(MythUIButtonListItem *item);
    void textChanged(void);
    void clauseChanged(MythUIButtonListItem *item);
    void clauseClicked(MythUIButtonListItem *item);
    void testClicked(void);
    void recordClicked(void);
    void storeClicked(void);
//    void cancelClicked(void);

  private:
    void loadData(void);
    void loadClauses(void);
    bool checkSyntax(void);
    void storeRule(bool is_search, bool is_new);
    void deleteRule(void);

    ProgramInfo *m_pginfo;

    int prevItem;
    int maxex;

    QString seSuffix;
    QString exSuffix;

    MythUIButtonList *m_ruleList;
    MythUIButtonList *m_clauseList;

    MythUITextEdit *m_titleEdit;

    // Contains the SQL statement
    MythUITextEdit *m_descriptionEdit;

    // Contains the additional SQL tables
    MythUITextEdit *m_subtitleEdit;

    MythUIText   *m_clauseText;
    MythUIButton *m_testButton;
    MythUIButton *m_recordButton;
    MythUIButton *m_storeButton;
    MythUIButton *m_cancelButton;
};

struct CustomRuleInfo {
    QString recordid;
    QString title;
    QString subtitle;
    QString description;
};

Q_DECLARE_METATYPE(CustomRuleInfo)

#endif