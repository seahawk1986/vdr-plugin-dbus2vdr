#include "publish.h"

#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/timeval.h>

#include <vdr/tools.h>

cAvahiPublish::cAvahiPublish(const char *name, const char *type, int port, int subtypes_len, const char **subtypes, int txts_len, const char **txts)
 :_simple_poll(NULL)
 ,_client(NULL)
 ,_group(NULL)
 ,_name(NULL)
 ,_type(NULL)
 ,_port(port)
 ,_subtypes(NULL)
 ,_txts(NULL)
 ,_started(false)
{
  _type = avahi_strdup(type);
  Modify(name, port, subtypes_len, subtypes, txts_len, txts);
  Start();
}

cAvahiPublish::~cAvahiPublish(void)
{
  Stop();
  if (_txts != NULL)
     avahi_string_list_free(_txts);
  _txts = NULL;
  if (_subtypes != NULL)
     avahi_string_list_free(_subtypes);
  _subtypes = NULL;
  if (_type != NULL)
     avahi_free(_type);
  _type = NULL;
  if (_name != NULL)
     avahi_free(_name);
  _name = NULL;
}

void cAvahiPublish::Modify(const char *name, int port, int subtypes_len, const char **subtypes, int txts_len, const char **txts)
{
  Lock();
  AvahiStringList *tmp_subtypes = NULL;
  AvahiStringList *tmp_txts = NULL;
  if (subtypes_len > 0)
     tmp_subtypes = avahi_string_list_new_from_array(subtypes, subtypes_len);
  if (txts_len > 0)
     tmp_txts = avahi_string_list_new_from_array(txts, txts_len);
  if ((_name != NULL) && (strcmp(_name, name) == 0) && (port == _port)
   && (avahi_string_list_equal(_subtypes, tmp_subtypes) == 0)
   && (avahi_string_list_equal(_txts, tmp_txts) == 0)) {
     if (tmp_subtypes != NULL)
        avahi_string_list_free(tmp_subtypes);
     if (tmp_txts != NULL)
        avahi_string_list_free(tmp_txts);
     Unlock();
     return;
     }
  if (_name != NULL)
     avahi_free(_name);
  _name = avahi_strdup(name);
  _port = port;
  if (_subtypes != NULL)
     avahi_string_list_free(_subtypes);
  _subtypes = tmp_subtypes;
  if (_txts != NULL)
     avahi_string_list_free(_txts);
  _txts = tmp_txts;
  if (_client != NULL) {
     struct timeval tv;
     avahi_simple_poll_get(_simple_poll)->timeout_new(
        avahi_simple_poll_get(_simple_poll),
        avahi_elapse_time(&tv, 1, 0),
        ModifyCallback, this);
     }
  Unlock();
  if (!Running())
     Start();
}

void cAvahiPublish::ModifyCallback(AvahiTimeout *e, void *userdata)
{
  if (userdata == NULL)
     return;
  ((cAvahiPublish*)userdata)->ModifyCallback(e);
}

void cAvahiPublish::ModifyCallback(AvahiTimeout *e)
{
  if (_client == NULL) {
     if (_simple_poll != NULL)
        avahi_simple_poll_get(_simple_poll)->timeout_free(e);
     return;
     }
  if (avahi_client_get_state(_client) == AVAHI_CLIENT_S_RUNNING) {
     if (_group != NULL)
        avahi_entry_group_reset(_group);
     CreateServices(_client);
     }
  if (_simple_poll != NULL)
     avahi_simple_poll_get(_simple_poll)->timeout_free(e);
}

void cAvahiPublish::Stop(void)
{
  Cancel(-1);
  Lock();
  if (_simple_poll != NULL)
     avahi_simple_poll_quit(_simple_poll);
  Unlock();
  Cancel(5);
}

void cAvahiPublish::ClientCallback(AvahiClient *client, AvahiClientState state, void *userdata)
{
  if (userdata == NULL)
     return;
  ((cAvahiPublish*)userdata)->ClientCallback(client, state);
}

void cAvahiPublish::ClientCallback(AvahiClient *client, AvahiClientState state)
{
  switch (state) {
    case AVAHI_CLIENT_S_RUNNING:
     {
       CreateServices(client);
       break;
     }
    case AVAHI_CLIENT_FAILURE:
     {
       esyslog("dbus2vdr/avahi: client failure: %s", avahi_strerror(avahi_client_errno(client)));
       avahi_simple_poll_quit(_simple_poll);
       break;
     }
    case AVAHI_CLIENT_S_COLLISION:
    case AVAHI_CLIENT_S_REGISTERING:
     {
       if (_group)
          avahi_entry_group_reset(_group);
       break;
     }
    case AVAHI_CLIENT_CONNECTING:
       break;
    }
}

void cAvahiPublish::CreateServices(AvahiClient *client)
{
  char *n;
  int ret;

  if (_group == NULL) {
      _group = avahi_entry_group_new(client, GroupCallback, this);
      if (_group == NULL) {
         esyslog("dbus2vdr/avahi: avahi_entry_group_new failed: %s", avahi_strerror(avahi_client_errno(client)));
         goto fail;
         }
      }
  if (avahi_entry_group_is_empty(_group)) {
     ret = avahi_entry_group_add_service(_group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, (AvahiPublishFlags)0, _name, _type, NULL, NULL, _port, NULL);
     if (ret  < 0) {
        if (ret == AVAHI_ERR_COLLISION)
            goto collision;
        esyslog("dbus2vdr/avahi: failed to add service '%s' of type '%s': %s", _name, _type, avahi_strerror(ret));
        goto fail;
        }

     if ((_subtypes != NULL) && (avahi_string_list_length(_subtypes) > 0)) {
        AvahiStringList *l = _subtypes;
        while (l != NULL) {
              const char *subtype = (const char*)avahi_string_list_get_text(l);
              size_t sublen = avahi_string_list_get_size(l);
              if ((subtype != NULL) && (sublen > 0) && (subtype[0] != 0)) {
                 ret = avahi_entry_group_add_service_subtype(_group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, (AvahiPublishFlags)0, _name, _type, NULL, subtype);
                 if (ret < 0)
                    esyslog("dbus2vdr/avahi: failed to add subtype %s on '%s' of type '%s': %s", subtype, _name, _type, avahi_strerror(ret));
                 }
              l = avahi_string_list_get_next(l);
              }
        }

     if ((_txts != NULL) && (avahi_string_list_length(_txts) > 0)) {
        ret = avahi_entry_group_update_service_txt_strlst(_group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, (AvahiPublishFlags)0, _name, _type, NULL, _txts);
        if (ret < 0)
           esyslog("dbus2vdr/avahi: failed to add txt records on '%s' of type '%s': %s", _name, _type, avahi_strerror(ret));
        }

     ret = avahi_entry_group_commit(_group);
     if (ret < 0) {
        esyslog("dbus2vdr/avahi: failed to commit entry group of '%s': %s", _name, avahi_strerror(ret));
        goto fail;
        }
     }
  return;

collision:
  n = avahi_alternative_service_name(_name);
  avahi_free(_name);
  _name = n;
  isyslog("dbus2vdr/avahi: service name collision, renaming service to '%s'", _name);
  avahi_entry_group_reset(_group);
  CreateServices(client);
  return;

fail:
  if (_group != NULL)
     avahi_entry_group_free(_group);
  _group = NULL;
  avahi_simple_poll_quit(_simple_poll);
}

void cAvahiPublish::GroupCallback(AvahiEntryGroup *group, AvahiEntryGroupState state, void *userdata)
{
  if (userdata == NULL)
     return;
  ((cAvahiPublish*)userdata)->GroupCallback(group, state);
}

void cAvahiPublish::GroupCallback(AvahiEntryGroup *group, AvahiEntryGroupState state)
{
  switch (state) {
    case AVAHI_ENTRY_GROUP_ESTABLISHED:
     {
       isyslog("dbus2vdr/avahi: service '%s' successfully established", _name);
       break;
     }
    case AVAHI_ENTRY_GROUP_COLLISION:
     {
       char *n;
       n = avahi_alternative_service_name(_name);
       avahi_free(_name);
       _name = n;
       isyslog("dbus2vdr/avahi: service name collision, renaming service to '%s'", _name);
       CreateServices(avahi_entry_group_get_client(group));
       break;
     }
    case AVAHI_ENTRY_GROUP_FAILURE:
     {
       esyslog("dbus2vdr/avahi: entry group failure: %s", avahi_strerror(avahi_client_errno(avahi_entry_group_get_client(group))));
       avahi_simple_poll_quit(_simple_poll);
       break;
     }
    case AVAHI_ENTRY_GROUP_UNCOMMITED:
    case AVAHI_ENTRY_GROUP_REGISTERING:
       break;
    }
}

void cAvahiPublish::Action(void)
{
  _started = true;
  isyslog("dbus2vdr/avahi: publisher started for %s, type = %s, port = %d", _name, _type, _port);

  int avahiError = 0;
  int reconnectLogCount = 0;
  while (Running()) {
        if (_simple_poll == NULL) {
           // don't get too verbose...
           if (reconnectLogCount < 5)
              isyslog("dbus2vdr/avahi: create simple_poll");
           else if (reconnectLogCount > 15) // ...and too quiet
              reconnectLogCount = 0;

           Lock();
           _simple_poll = avahi_simple_poll_new();
           if (_simple_poll == NULL) {
              Unlock();
              esyslog("dbus2vdr/avahi: error on creating simple_poll");
              cCondWait::SleepMs(1000);
              reconnectLogCount++;
              continue;
              }
           Unlock();
           reconnectLogCount = 0;
           }

        if (_client == NULL) {
           // don't get too verbose...
           if (reconnectLogCount < 5)
              isyslog("dbus2vdr/avahi: create client");
           else if (reconnectLogCount > 15) // ...and too quiet
              reconnectLogCount = 0;

           Lock();
           _client = avahi_client_new(avahi_simple_poll_get(_simple_poll), AVAHI_CLIENT_NO_FAIL, ClientCallback, this, &avahiError);
           if (_client == NULL) {
              Unlock();
              esyslog("dbus2vdr/avahi: error on creating client: %s", avahi_strerror(avahiError));
              cCondWait::SleepMs(1000);
              reconnectLogCount++;
              continue;
              }
           Unlock();
           reconnectLogCount = 0;
           }

        avahi_simple_poll_loop(_simple_poll);

        Lock();
        if (_group != NULL)
           avahi_entry_group_free(_group);
        _group = NULL;
        if (_client != NULL)
           avahi_client_free(_client);
        _client = NULL;
        if (_simple_poll != NULL)
           avahi_simple_poll_free(_simple_poll);
        _simple_poll = NULL;
        Unlock();
        }
}