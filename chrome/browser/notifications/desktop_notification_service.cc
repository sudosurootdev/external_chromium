// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/notifications/desktop_notification_service.h"

#include "app/l10n_util.h"
#include "app/resource_bundle.h"
#include "base/thread.h"
#include "base/utf_string_conversions.h"
#include "chrome/browser/browser_child_process_host.h"
#include "chrome/browser/chrome_thread.h"
#include "chrome/browser/extensions/extensions_service.h"
#include "chrome/browser/notifications/notification.h"
#include "chrome/browser/notifications/notification_object_proxy.h"
#include "chrome/browser/notifications/notification_ui_manager.h"
#include "chrome/browser/notifications/notifications_prefs_cache.h"
#include "chrome/browser/pref_service.h"
#include "chrome/browser/profile.h"
#include "chrome/browser/renderer_host/render_process_host.h"
#include "chrome/browser/renderer_host/render_view_host.h"
#include "chrome/browser/renderer_host/site_instance.h"
#include "chrome/browser/scoped_pref_update.h"
#include "chrome/browser/tab_contents/infobar_delegate.h"
#include "chrome/browser/tab_contents/tab_contents.h"
#include "chrome/browser/worker_host/worker_process_host.h"
#include "chrome/common/notification_service.h"
#include "chrome/common/pref_names.h"
#include "chrome/common/render_messages.h"
#include "chrome/common/url_constants.h"
#include "grit/browser_resources.h"
#include "grit/chromium_strings.h"
#include "grit/generated_resources.h"
#include "grit/theme_resources.h"
#include "net/base/escape.h"
#include "third_party/WebKit/WebKit/chromium/public/WebNotificationPresenter.h"

using WebKit::WebNotificationPresenter;
using WebKit::WebTextDirection;

const ContentSetting kDefaultSetting = CONTENT_SETTING_ASK;

// static
string16 DesktopNotificationService::CreateDataUrl(
    const GURL& icon_url, const string16& title, const string16& body,
    WebTextDirection dir) {
  int resource;
  string16 line_name;
  string16 line;
  std::vector<std::string> subst;
  if (icon_url.is_valid()) {
    resource = IDR_NOTIFICATION_ICON_HTML;
    subst.push_back(icon_url.spec());
    subst.push_back(EscapeForHTML(UTF16ToUTF8(title)));
    subst.push_back(EscapeForHTML(UTF16ToUTF8(body)));
    // icon float position
    subst.push_back(dir == WebKit::WebTextDirectionRightToLeft ?
                    "right" : "left");
  } else if (title.empty() || body.empty()) {
    resource = IDR_NOTIFICATION_1LINE_HTML;
    line = title.empty() ? body : title;
    // Strings are div names in the template file.
    line_name = title.empty() ? ASCIIToUTF16("description")
                              : ASCIIToUTF16("title");
    subst.push_back(EscapeForHTML(UTF16ToUTF8(line_name)));
    subst.push_back(EscapeForHTML(UTF16ToUTF8(line)));
  } else {
    resource = IDR_NOTIFICATION_2LINE_HTML;
    subst.push_back(EscapeForHTML(UTF16ToUTF8(title)));
    subst.push_back(EscapeForHTML(UTF16ToUTF8(body)));
  }
  // body text direction
  subst.push_back(dir == WebKit::WebTextDirectionRightToLeft ?
                  "rtl" : "ltr");

  const base::StringPiece template_html(
      ResourceBundle::GetSharedInstance().GetRawDataResource(
          resource));

  if (template_html.empty()) {
    NOTREACHED() << "unable to load template. ID: " << resource;
    return string16();
  }

  std::string data = ReplaceStringPlaceholders(template_html, subst, NULL);
  return UTF8ToUTF16("data:text/html;charset=utf-8," +
                      EscapeQueryParamValue(data, false));
}

// A task object which calls the renderer to inform the web page that the
// permission request has completed.
class NotificationPermissionCallbackTask : public Task {
 public:
  NotificationPermissionCallbackTask(int process_id, int route_id,
      int request_id)
      : process_id_(process_id),
        route_id_(route_id),
        request_id_(request_id) {
  }

  virtual void Run() {
    DCHECK(ChromeThread::CurrentlyOn(ChromeThread::IO));
    RenderViewHost* host = RenderViewHost::FromID(process_id_, route_id_);
    if (host)
      host->Send(new ViewMsg_PermissionRequestDone(route_id_, request_id_));
  }

 private:
  int process_id_;
  int route_id_;
  int request_id_;
};

// The delegate for the infobar shown when an origin requests notification
// permissions.
class NotificationPermissionInfoBarDelegate : public ConfirmInfoBarDelegate {
 public:
  NotificationPermissionInfoBarDelegate(TabContents* contents,
                                        const GURL& origin,
                                        const std::wstring& display_name,
                                        int process_id,
                                        int route_id,
                                        int callback_context)
      : ConfirmInfoBarDelegate(contents),
        origin_(origin),
        display_name_(display_name),
        profile_(contents->profile()),
        process_id_(process_id),
        route_id_(route_id),
        callback_context_(callback_context),
        action_taken_(false) {
  }

  // Overridden from ConfirmInfoBarDelegate:
  virtual void InfoBarClosed() {
    if (!action_taken_)
      UMA_HISTOGRAM_COUNTS("NotificationPermissionRequest.Ignored", 1);

    ChromeThread::PostTask(
      ChromeThread::IO, FROM_HERE,
      new NotificationPermissionCallbackTask(
          process_id_, route_id_, callback_context_));

    delete this;
  }

  virtual std::wstring GetMessageText() const {
    return l10n_util::GetStringF(IDS_NOTIFICATION_PERMISSIONS, display_name_);
  }

  virtual SkBitmap* GetIcon() const {
    return ResourceBundle::GetSharedInstance().GetBitmapNamed(
       IDR_PRODUCT_ICON_32);
  }

  virtual int GetButtons() const {
    return BUTTON_OK | BUTTON_CANCEL | BUTTON_OK_DEFAULT;
  }

  virtual std::wstring GetButtonLabel(InfoBarButton button) const {
    return button == BUTTON_OK ?
        l10n_util::GetString(IDS_NOTIFICATION_PERMISSION_YES) :
        l10n_util::GetString(IDS_NOTIFICATION_PERMISSION_NO);
  }

  virtual bool Accept() {
    UMA_HISTOGRAM_COUNTS("NotificationPermissionRequest.Allowed", 1);
    profile_->GetDesktopNotificationService()->GrantPermission(origin_);
    action_taken_ = true;
    return true;
  }

  virtual bool Cancel() {
    UMA_HISTOGRAM_COUNTS("NotificationPermissionRequest.Denied", 1);
    profile_->GetDesktopNotificationService()->DenyPermission(origin_);
    action_taken_ = true;
    return true;
  }

 private:
  // The origin we are asking for permissions on.
  GURL origin_;

  // The display name for the origin to be displayed.  Will be different from
  // origin_ for extensions.
  std::wstring display_name_;

  // The Profile that we restore sessions from.
  Profile* profile_;

  // The callback information that tells us how to respond to javascript via
  // the correct RenderView.
  int process_id_;
  int route_id_;
  int callback_context_;

  // Whether the user clicked one of the buttons.
  bool action_taken_;

  DISALLOW_COPY_AND_ASSIGN(NotificationPermissionInfoBarDelegate);
};

DesktopNotificationService::DesktopNotificationService(Profile* profile,
    NotificationUIManager* ui_manager)
    : profile_(profile),
      ui_manager_(ui_manager) {
  InitPrefs();
  StartObserving();
}

DesktopNotificationService::~DesktopNotificationService() {
  StopObserving();
}

void DesktopNotificationService::RegisterUserPrefs(PrefService* user_prefs) {
  if (!user_prefs->FindPreference(
      prefs::kDesktopNotificationDefaultContentSetting)) {
    user_prefs->RegisterIntegerPref(
        prefs::kDesktopNotificationDefaultContentSetting, kDefaultSetting);
  }
  if (!user_prefs->FindPreference(prefs::kDesktopNotificationAllowedOrigins))
    user_prefs->RegisterListPref(prefs::kDesktopNotificationAllowedOrigins);
  if (!user_prefs->FindPreference(prefs::kDesktopNotificationDeniedOrigins))
    user_prefs->RegisterListPref(prefs::kDesktopNotificationDeniedOrigins);
}

// Initialize the cache with the allowed and denied origins, or
// create the preferences if they don't exist yet.
void DesktopNotificationService::InitPrefs() {
  PrefService* prefs = profile_->GetPrefs();
  std::vector<GURL> allowed_origins;
  std::vector<GURL> denied_origins;
  ContentSetting default_content_setting = CONTENT_SETTING_DEFAULT;

  if (!profile_->IsOffTheRecord()) {
    default_content_setting = IntToContentSetting(
        prefs->GetInteger(prefs::kDesktopNotificationDefaultContentSetting));
    allowed_origins = GetAllowedOrigins();
    denied_origins = GetBlockedOrigins();
  }

  prefs_cache_ = new NotificationsPrefsCache();
  prefs_cache_->SetCacheDefaultContentSetting(default_content_setting);
  prefs_cache_->SetCacheAllowedOrigins(allowed_origins);
  prefs_cache_->SetCacheDeniedOrigins(denied_origins);
  prefs_cache_->set_is_initialized(true);
}

void DesktopNotificationService::StartObserving() {
  if (!profile_->IsOffTheRecord()) {
    PrefService* prefs = profile_->GetPrefs();
    prefs->AddPrefObserver(prefs::kDesktopNotificationDefaultContentSetting,
                           this);
    prefs->AddPrefObserver(prefs::kDesktopNotificationAllowedOrigins, this);
    prefs->AddPrefObserver(prefs::kDesktopNotificationDeniedOrigins, this);
  }
}

void DesktopNotificationService::StopObserving() {
  if (!profile_->IsOffTheRecord()) {
    PrefService* prefs = profile_->GetPrefs();
    prefs->RemovePrefObserver(prefs::kDesktopNotificationDefaultContentSetting,
                              this);
    prefs->RemovePrefObserver(prefs::kDesktopNotificationAllowedOrigins, this);
    prefs->RemovePrefObserver(prefs::kDesktopNotificationDeniedOrigins, this);
  }
}

void DesktopNotificationService::GrantPermission(const GURL& origin) {
  DCHECK(ChromeThread::CurrentlyOn(ChromeThread::UI));
  PersistPermissionChange(origin, true);

  // Schedule a cache update on the IO thread.
  ChromeThread::PostTask(
      ChromeThread::IO, FROM_HERE,
      NewRunnableMethod(
          prefs_cache_.get(), &NotificationsPrefsCache::CacheAllowedOrigin,
          origin));
}

void DesktopNotificationService::DenyPermission(const GURL& origin) {
  DCHECK(ChromeThread::CurrentlyOn(ChromeThread::UI));
  PersistPermissionChange(origin, false);

  // Schedule a cache update on the IO thread.
  ChromeThread::PostTask(
      ChromeThread::IO, FROM_HERE,
      NewRunnableMethod(
          prefs_cache_.get(), &NotificationsPrefsCache::CacheDeniedOrigin,
          origin));
}

void DesktopNotificationService::Observe(NotificationType type,
                                         const NotificationSource& source,
                                         const NotificationDetails& details) {
  DCHECK(NotificationType::PREF_CHANGED == type);
  PrefService* prefs = profile_->GetPrefs();
  std::wstring* name = Details<std::wstring>(details).ptr();

  if (0 == name->compare(prefs::kDesktopNotificationAllowedOrigins)) {
    std::vector<GURL> allowed_origins(GetAllowedOrigins());
    // Schedule a cache update on the IO thread.
    ChromeThread::PostTask(
        ChromeThread::IO, FROM_HERE,
        NewRunnableMethod(
            prefs_cache_.get(),
            &NotificationsPrefsCache::SetCacheAllowedOrigins,
            allowed_origins));
  } else if (0 == name->compare(prefs::kDesktopNotificationDeniedOrigins)) {
    std::vector<GURL> denied_origins(GetBlockedOrigins());
    // Schedule a cache update on the IO thread.
    ChromeThread::PostTask(
        ChromeThread::IO, FROM_HERE,
        NewRunnableMethod(
            prefs_cache_.get(),
            &NotificationsPrefsCache::SetCacheDeniedOrigins,
            denied_origins));
  } else if (0 == name->compare(
      prefs::kDesktopNotificationDefaultContentSetting)) {
    const ContentSetting default_content_setting = IntToContentSetting(
        prefs->GetInteger(prefs::kDesktopNotificationDefaultContentSetting));

    // Schedule a cache update on the IO thread.
    ChromeThread::PostTask(
        ChromeThread::IO, FROM_HERE,
        NewRunnableMethod(
            prefs_cache_.get(),
            &NotificationsPrefsCache::SetCacheDefaultContentSetting,
            default_content_setting));
  }
}

void DesktopNotificationService::PersistPermissionChange(
    const GURL& origin, bool is_allowed) {
  // Don't persist changes when off the record.
  if (profile_->IsOffTheRecord())
    return;

  PrefService* prefs = profile_->GetPrefs();

  // |Observe()| updates the whole permission set in the cache, but only a
  // single origin has changed. Hence, callers of this method manually
  // schedule a task to update the prefs cache, and the prefs observer is
  // disabled while the update runs.
  StopObserving();

  bool allowed_changed = false;
  bool denied_changed = false;

  ListValue* allowed_sites =
      prefs->GetMutableList(prefs::kDesktopNotificationAllowedOrigins);
  ListValue* denied_sites =
      prefs->GetMutableList(prefs::kDesktopNotificationDeniedOrigins);
  {
    // value is passed to the preferences list, or deleted.
    StringValue* value = new StringValue(origin.spec());

    // Remove from one list and add to the other.
    if (is_allowed) {
      // Remove from the denied list.
      if (denied_sites->Remove(*value) != -1)
        denied_changed = true;

      // Add to the allowed list.
      if (allowed_sites->AppendIfNotPresent(value))
        allowed_changed = true;
      else
        delete value;
    } else {
      // Remove from the allowed list.
      if (allowed_sites->Remove(*value) != -1)
        allowed_changed = true;

      // Add to the denied list.
      if (denied_sites->AppendIfNotPresent(value))
        denied_changed = true;
      else
        delete value;
    }
  }

  // Persist the pref if anthing changed, but only send updates for the
  // list that changed.
  if (allowed_changed || denied_changed) {
    if (allowed_changed) {
      ScopedPrefUpdate update_allowed(
          prefs, prefs::kDesktopNotificationAllowedOrigins);
    }
    if (denied_changed) {
      ScopedPrefUpdate updateDenied(
          prefs, prefs::kDesktopNotificationDeniedOrigins);
    }
    prefs->ScheduleSavePersistentPrefs();
  }
  StartObserving();
}

ContentSetting DesktopNotificationService::GetDefaultContentSetting() {
  PrefService* prefs = profile_->GetPrefs();
  ContentSetting setting = IntToContentSetting(
      prefs->GetInteger(prefs::kDesktopNotificationDefaultContentSetting));
  if (setting == CONTENT_SETTING_DEFAULT)
    setting = kDefaultSetting;
  return setting;
}

void DesktopNotificationService::SetDefaultContentSetting(
    ContentSetting setting) {
  DCHECK(ChromeThread::CurrentlyOn(ChromeThread::UI));
  profile_->GetPrefs()->SetInteger(
      prefs::kDesktopNotificationDefaultContentSetting,
      setting == CONTENT_SETTING_DEFAULT ?  kDefaultSetting : setting);
  // The cache is updated through the notification observer.
}

std::vector<GURL> DesktopNotificationService::GetAllowedOrigins() {
  std::vector<GURL> allowed_origins;
  PrefService* prefs = profile_->GetPrefs();
  const ListValue* allowed_sites =
      prefs->GetList(prefs::kDesktopNotificationAllowedOrigins);
  if (allowed_sites) {
    NotificationsPrefsCache::ListValueToGurlVector(*allowed_sites,
                                                   &allowed_origins);
  }
  return allowed_origins;
}

std::vector<GURL> DesktopNotificationService::GetBlockedOrigins() {
  std::vector<GURL> denied_origins;
  PrefService* prefs = profile_->GetPrefs();
  const ListValue* denied_sites =
      prefs->GetList(prefs::kDesktopNotificationDeniedOrigins);
  if (denied_sites) {
    NotificationsPrefsCache::ListValueToGurlVector(*denied_sites,
                                                   &denied_origins);
  }
  return denied_origins;
}

void DesktopNotificationService::ResetAllowedOrigin(const GURL& origin) {
  if (profile_->IsOffTheRecord())
    return;

  // Since this isn't called often, let the normal observer behavior update the
  // cache in this case.
  PrefService* prefs = profile_->GetPrefs();
  ListValue* allowed_sites =
      prefs->GetMutableList(prefs::kDesktopNotificationAllowedOrigins);
  {
    StringValue value(origin.spec());
    int removed_index = allowed_sites->Remove(value);
    DCHECK_NE(-1, removed_index) << origin << " was not allowed";
    ScopedPrefUpdate update_allowed(
        prefs, prefs::kDesktopNotificationAllowedOrigins);
  }
  prefs->ScheduleSavePersistentPrefs();
}

void DesktopNotificationService::ResetBlockedOrigin(const GURL& origin) {
  if (profile_->IsOffTheRecord())
    return;

  // Since this isn't called often, let the normal observer behavior update the
  // cache in this case.
  PrefService* prefs = profile_->GetPrefs();
  ListValue* denied_sites =
      prefs->GetMutableList(prefs::kDesktopNotificationDeniedOrigins);
  {
    StringValue value(origin.spec());
    int removed_index = denied_sites->Remove(value);
    DCHECK_NE(-1, removed_index) << origin << " was not blocked";
    ScopedPrefUpdate update_allowed(
        prefs, prefs::kDesktopNotificationDeniedOrigins);
  }
  prefs->ScheduleSavePersistentPrefs();
}

void DesktopNotificationService::ResetAllOrigins() {
  PrefService* prefs = profile_->GetPrefs();
  prefs->ClearPref(prefs::kDesktopNotificationAllowedOrigins);
  prefs->ClearPref(prefs::kDesktopNotificationDeniedOrigins);
}

ContentSetting DesktopNotificationService::GetContentSetting(
    const GURL& origin) {
  DCHECK(ChromeThread::CurrentlyOn(ChromeThread::UI));
  if (profile_->IsOffTheRecord())
    return kDefaultSetting;

  std::vector<GURL> allowed_origins(GetAllowedOrigins());
  if (std::find(allowed_origins.begin(), allowed_origins.end(), origin) !=
      allowed_origins.end())
    return CONTENT_SETTING_ALLOW;

  std::vector<GURL> denied_origins(GetBlockedOrigins());
  if (std::find(denied_origins.begin(), denied_origins.end(), origin) !=
      denied_origins.end())
    return CONTENT_SETTING_BLOCK;

  return GetDefaultContentSetting();
}

void DesktopNotificationService::RequestPermission(
    const GURL& origin, int process_id, int route_id, int callback_context,
    TabContents* tab) {
  DCHECK(ChromeThread::CurrentlyOn(ChromeThread::UI));
  if (!tab)
    return;

  // If |origin| hasn't been seen before and the default content setting for
  // notifications is "ask", show an infobar.
  // The cache can only answer queries on the IO thread once it's initialized,
  // so don't ask the cache.
  ContentSetting setting = GetContentSetting(origin);
  if (setting == CONTENT_SETTING_ASK) {
    // Show an info bar requesting permission.
    std::wstring display_name = DisplayNameForOrigin(origin);

    tab->AddInfoBar(new NotificationPermissionInfoBarDelegate(
        tab, origin, display_name, process_id, route_id, callback_context));
  } else {
    // Notify renderer immediately.
    ChromeThread::PostTask(
      ChromeThread::IO, FROM_HERE,
      new NotificationPermissionCallbackTask(
          process_id, route_id, callback_context));
  }
}

void DesktopNotificationService::ShowNotification(
    const Notification& notification) {
  ui_manager_->Add(notification, profile_);
}

bool DesktopNotificationService::CancelDesktopNotification(
    int process_id, int route_id, int notification_id) {
  scoped_refptr<NotificationObjectProxy> proxy(
      new NotificationObjectProxy(process_id, route_id, notification_id,
                                  false));
  // TODO(johnnyg): clean up this "empty" notification.
  Notification notif(GURL(), GURL(), L"", ASCIIToUTF16(""), proxy);
  return ui_manager_->Cancel(notif);
}


bool DesktopNotificationService::ShowDesktopNotification(
    const ViewHostMsg_ShowNotification_Params& params,
    int process_id, int route_id, DesktopNotificationSource source) {
  DCHECK(ChromeThread::CurrentlyOn(ChromeThread::UI));
  const GURL& origin = params.origin;
  NotificationObjectProxy* proxy =
      new NotificationObjectProxy(process_id, route_id,
                                  params.notification_id,
                                  source == WorkerNotification);
  GURL contents;
  if (params.is_html) {
    contents = params.contents_url;
  } else {
    // "upconvert" the string parameters to a data: URL.
    contents = GURL(
        CreateDataUrl(params.icon_url, params.title, params.body,
                      params.direction));
  }
  Notification notif(
      origin, contents, DisplayNameForOrigin(origin), params.replace_id, proxy);
  ShowNotification(notif);
  return true;
}

std::wstring DesktopNotificationService::DisplayNameForOrigin(
    const GURL& origin) {
  // If the source is an extension, lookup the display name.
  if (origin.SchemeIs(chrome::kExtensionScheme)) {
    ExtensionsService* ext_service = profile_->GetExtensionsService();
    if (ext_service) {
      Extension* extension = ext_service->GetExtensionByURL(origin);
      if (extension)
        return UTF8ToWide(extension->name());
    }
  }
  return UTF8ToWide(origin.host());
}