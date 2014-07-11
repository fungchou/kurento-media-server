/*
 * (C) Copyright 2013 Kurento (http://kurento.org/)
 *
 * All rights reserved. This program and the accompanying materials
 * are made available under the terms of the GNU Lesser General Public License
 * (LGPL) version 2.1 which accompanies this distribution, and is available at
 * http://www.gnu.org/licenses/lgpl-2.1.html
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 */

#include <config.h>

#include "media_config.hpp"

#include <glibmm.h>
#include <fstream>
#include <iostream>
#include <boost/filesystem.hpp>
#include <version.hpp>
#include <ModuleLoader.hpp>
#include <glib/gstdio.h>
#include <ftw.h>

#include "services/Service.hpp"
#include "services/ServiceFactory.hpp"

#include <SignalHandler.hpp>

#define GST_CAT_DEFAULT kurento_media_server
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);
#define GST_DEFAULT_NAME "KurentoMediaServer"

#define ENV_VAR "GST_PLUGIN_PATH"

#define FILE_PERMISIONS (S_IRWXU | S_IRWXG | S_IRWXO)
#define TMP_DIR_TEMPLATE "/tmp/kms_XXXXXX"
#define CERTTOOL_TEMPLATE "autoCerttool.tmpl"
#define CERT_KEY_PEM_FILE "autoCertkey.pem"

using namespace ::kurento;

using boost::shared_ptr;
using namespace boost::filesystem;

using ::Glib::KeyFile;
using ::Glib::KeyFileFlags;

static Service *service;

GstSDPMessage *sdpPattern;
std::string stunServerAddress, turnURL, pemCertificate;
__pid_t pid;
gint stunServerPort;

Glib::RefPtr<Glib::MainLoop> loop = Glib::MainLoop::create ();

static gchar *conf_file;
static gchar *tmp_dir;

static GOptionEntry entries[] = {
  {
    "conf-file", 'f', 0, G_OPTION_ARG_FILENAME, &conf_file, "Configuration file",
    NULL
  },
  {NULL}
};

Glib::RefPtr<Glib::IOChannel> channel;

static void
check_port (int port)
{
  if (port <= 0 || port > G_MAXUSHORT) {
    throw Glib::KeyFileError (Glib::KeyFileError::PARSE, "Invalid value");
  }
}

static gchar *
read_entire_file (const gchar *file_name)
{
  gchar *data;
  long f_size;
  FILE *fp;

  fp = fopen (file_name, "r");

  if (fp == NULL) {
    return NULL;
  }

  fseek (fp, 0, SEEK_END);
  f_size = ftell (fp);
  fseek (fp, 0, SEEK_SET);
  data = (gchar *) g_malloc0 (f_size + 1);

  if (fread (data, 1, f_size, fp) != (size_t) f_size) {
    GST_ERROR ("Error reading file");
  }

  fclose (fp);

  data[f_size] = '\0';

  return data;
}

static GstSDPMessage *
load_sdp_pattern (Glib::KeyFile &configFile, const std::string &confFileName)
{
  GstSDPResult result;
  GstSDPMessage *sdp_pattern = NULL;
  gchar *sdp_pattern_text;
  std::string sdp_pattern_file_name;

  GST_DEBUG ("Load SDP Pattern");
  result = gst_sdp_message_new (&sdp_pattern);

  if (result != GST_SDP_OK) {
    GST_ERROR ("Error creating sdp message");
    return NULL;
  }

  sdp_pattern_file_name = configFile.get_string (SERVER_GROUP, SDP_PATTERN_KEY);
  boost::filesystem::path p (confFileName.c_str () );
  sdp_pattern_file_name.insert (0, "/");
  sdp_pattern_file_name.insert (0, p.parent_path ().c_str() );
  sdp_pattern_text = read_entire_file (sdp_pattern_file_name.c_str () );

  if (sdp_pattern_text == NULL) {
    GST_ERROR ("Error reading SDP pattern file");
    gst_sdp_message_free (sdp_pattern);
    return NULL;
  }

  result = gst_sdp_message_parse_buffer ( (const guint8 *) sdp_pattern_text, -1,
                                          sdp_pattern);
  g_free (sdp_pattern_text);

  if (result != GST_SDP_OK) {
    GST_ERROR ("Error parsing SDP config pattern");
    gst_sdp_message_free (sdp_pattern);
    return NULL;
  }

  return sdp_pattern;
}

static void
configure_kurento_media_server (KeyFile &configFile,
                                const std::string &file_name)
{
  gchar *sdpMessageText = NULL;

  try {
    sdpPattern = load_sdp_pattern (configFile, file_name);
    GST_DEBUG ("SDP: \n%s", sdpMessageText = gst_sdp_message_as_text (sdpPattern) );
    g_free (sdpMessageText);
  } catch (const Glib::KeyFileError &err) {
    GST_ERROR ("%s", err.what ().c_str () );
    GST_WARNING ("Wrong codec configuration, communication won't be possible");
  }

  try {
    service = ServiceFactory::create_service (configFile);
  } catch (std::exception &e) {
    GST_ERROR ("Error creating service: %s", e.what() );
    exit (1);
  } catch (Glib::Exception &e) {
    GST_ERROR ("Error creating service: %s", e.what().c_str() );
    exit (1);
  }
}

static gchar *
generate_certkey_pem_file (const gchar *dir)
{
  gchar *cmd, *template_path, *pem_path;
  int ret;

  if (dir == NULL) {
    return NULL;
  }

  pem_path = g_strdup_printf ("%s/%s", dir, CERT_KEY_PEM_FILE);
  cmd =
    g_strconcat ("/bin/sh -c \"certtool --generate-privkey --outfile ",
                 pem_path, "\"", NULL);
  ret = system (cmd);
  g_free (cmd);

  if (ret == -1) {
    goto err;
  }

  template_path = g_strdup_printf ("%s/%s", dir, CERTTOOL_TEMPLATE);
  cmd =
    g_strconcat
    ("/bin/sh -c \"echo 'organization = kurento' > ", template_path,
     " && certtool --generate-self-signed --load-privkey ", pem_path,
     " --template ", template_path, " >> ", pem_path, " 2>/dev/null\"", NULL);
  g_free (template_path);
  ret = system (cmd);
  g_free (cmd);

  if (ret == -1) {
    goto err;
  }

  return pem_path;

err:

  GST_ERROR ("Error while generating certificate file");

  g_free (pem_path);
  return NULL;
}

static void
configure_web_rtc_end_point (KeyFile &configFile, const std::string &file_name)
{
  gint port;
  std::string pem_certificate_file_name;

  try {
    stunServerAddress = configFile.get_string (WEB_RTC_END_POINT_GROUP,
                        WEB_RTC_END_POINT_STUN_SERVER_ADDRESS_KEY);
  } catch (const Glib::KeyFileError &err) {
    GST_WARNING ("Setting default address %s to stun server",
                 STUN_SERVER_ADDRESS);
    stunServerAddress = STUN_SERVER_ADDRESS;
  }

  try {
    port = configFile.get_integer (WEB_RTC_END_POINT_GROUP,
                                   WEB_RTC_END_POINT_STUN_SERVER_PORT_KEY);
    check_port (port);
    stunServerPort = port;
  } catch (const Glib::KeyFileError &err) {
    GST_WARNING ("Setting default port %d to stun server",
                 STUN_SERVER_PORT);
    stunServerPort = STUN_SERVER_PORT;
  }

  try {
    turnURL = configFile.get_string (WEB_RTC_END_POINT_GROUP,
                                     WEB_RTC_END_POINT_TURN_URL_KEY);
  } catch (const Glib::KeyFileError &err) {
    GST_WARNING ("No TURN server configured");
  }

  try {
    pem_certificate_file_name = configFile.get_string (WEB_RTC_END_POINT_GROUP,
                                WEB_RTC_END_POINT_PEM_CERTIFICATE_KEY);
    boost::filesystem::path p (file_name.c_str () );
    pem_certificate_file_name.insert (0, "/");
    pem_certificate_file_name.insert (0, p.parent_path ().c_str() );
    pemCertificate = pem_certificate_file_name.c_str();

  } catch (const Glib::KeyFileError &err) {
    //generate a valid pem certificate
    gchar t[] = TMP_DIR_TEMPLATE;
    gchar *autogenerated_pem_file;

    tmp_dir = g_strdup (g_mkdtemp_full (t, FILE_PERMISIONS) );
    autogenerated_pem_file = generate_certkey_pem_file (tmp_dir);

    if (autogenerated_pem_file == NULL ) {
      pemCertificate = "";
      GST_ERROR ("%s", err.what ().c_str () );
      GST_WARNING ("Could not create Pem Certificate or Pem Certificate not found");
    } else {
      pemCertificate = autogenerated_pem_file;
    }
  }
}

static void
load_config (const std::string &file_name)
{
  KeyFile configFile;

  pid = getpid();

  GST_INFO ("Reading configuration from: %s", file_name.c_str () );

  try {
    configFile.load_from_file (file_name,
                               KeyFileFlags::KEY_FILE_KEEP_COMMENTS |
                               KeyFileFlags::KEY_FILE_KEEP_TRANSLATIONS);
  } catch (const Glib::Error &ex) {
    GST_ERROR ("Error loading configuration: %s", ex.what ().c_str () );
    throw ex;
  }

  /* parse options so as to configure services */
  configure_kurento_media_server (configFile, file_name);
  configure_web_rtc_end_point (configFile, file_name);

  GST_INFO ("Configuration loaded successfully");
}

static void
signal_handler (uint32_t signo)
{
  static unsigned int __terminated = 0;

  switch (signo) {
  case SIGINT:
  case SIGTERM:
    if (__terminated == 0) {
      GST_DEBUG ("Terminating.");
      loop->quit ();
    }

    __terminated = 1;
    break;

  case SIGPIPE:
    GST_DEBUG ("Ignore sigpipe signal");
    break;

  case SIGSEGV:
    GST_DEBUG ("Segmentation fault. Aborting process execution");
    abort ();

  default:
    break;
  }
}

static int
delete_file (const char *fpath, const struct stat *sb, int typeflag,
             struct FTW *ftwbuf)
{
  int rv = g_remove (fpath);

  if (rv) {
    GST_WARNING ("Error deleting file: %s. %s", fpath, strerror (errno) );
  }

  return rv;
}
static void
remove_recursive (const gchar *path)
{
  nftw (path, delete_file, 64, FTW_DEPTH | FTW_PHYS);
}

static void
deleteCertificate ()
{
  // Only parent process can delete certificate
  if (pid != getpid() ) {
    return;
  }

  if (tmp_dir != NULL) {
    remove_recursive (tmp_dir);
    g_free (tmp_dir);
  }
}

static void
check_if_plugins_are_available ()
{
  GstPlugin *plugin = gst_plugin_load_by_name ("kurento");

  if (plugin == NULL) {
    g_printerr ("Kurento plugin not found, try adding the plugins route with "
                "--gst-plugin-path parameter. See help (--help-gst) "
                "for more info\n");
    exit (1);
  }

  g_clear_object (&plugin);
}

int
main (int argc, char **argv)
{
  sigset_t mask;
  std::shared_ptr <SignalHandler> signalHandler;
  GError *error = NULL;
  GOptionContext *context;
  gchar *oldEnv, *newEnv;

  Glib::init();

  oldEnv = getenv (ENV_VAR);

  if (oldEnv == NULL) {
    newEnv = g_strdup_printf ("%s", PLUGIN_PATH);
  } else {
    newEnv = g_strdup_printf ("%s:%s", oldEnv, PLUGIN_PATH);
  }

  setenv (ENV_VAR, newEnv, 1);
  g_free (newEnv);

  gst_init (&argc, &argv);
  GST_DEBUG_CATEGORY_INIT (GST_CAT_DEFAULT, GST_DEFAULT_NAME, 0,
                           GST_DEFAULT_NAME);

  context = g_option_context_new ("");
  g_option_context_add_main_entries (context, entries, NULL);
  g_option_context_add_group (context, gst_init_get_option_group () );

  if (!g_option_context_parse (context, &argc, &argv, &error) ) {
    g_printerr ("option parsing failed: %s\n", error->message);
    g_option_context_free (context);
    g_error_free (error);
    exit (1);
  }

  g_option_context_free (context);

  check_if_plugins_are_available ();

  /* Install our signal handler */
  sigemptyset (&mask);
  sigaddset (&mask, SIGINT);
  sigaddset (&mask, SIGTERM);
  sigaddset (&mask, SIGSEGV);
  sigaddset (&mask, SIGPIPE);
  signalHandler = std::shared_ptr <SignalHandler> (new SignalHandler (mask,
                  signal_handler) );

  GST_INFO ("Kmsc version: %s", get_version () );

  if (!conf_file) {
    load_config (DEFAULT_CONFIG_FILE);
  } else {
    load_config ( (std::string) conf_file);
  }

  /* Start service */
  service->start ();

  loop->run ();

  signalHandler.reset();

  deleteCertificate ();

  service->stop();

  delete service;

  return 0;
}
