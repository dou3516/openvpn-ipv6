/*
 *  OpenVPN -- An application to securely tunnel IP networks
 *             over a single UDP port, with support for SSL/TLS-based
 *             session authentication and key exchange,
 *             packet encryption, packet authentication, and
 *             packet compression.
 *
 *  Copyright (C) 2002-2003 James Yonan <jim@yonan.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program (see the file COPYING included with this
 *  distribution); if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#ifdef WIN32
#include "config-win32.h"
#else
#include "config.h"
#endif

#include "syshead.h"

#include "common.h"
#include "error.h"
#include "options.h"
#include "socket.h"
#include "buffer.h"
#include "crypto.h"
#include "ssl.h"
#include "misc.h"
#include "lzo.h"
#include "tun.h"
#include "gremlin.h"
#include "shaper.h"
#include "thread.h"
#include "interval.h"
#include "fragment.h"
#include "openvpn.h"

#include "memdbg.h"

/*
 * Should we become a daemon?
 *  level == 0 after parameters have been parsed but before any initialization
 *  level == 1 after initialization but before any SSL/TLS negotiation or
 *    tunnel data is forwarded
 *  first_time is true until first exit of openvpn() function
 *
 * Return true if we did it.
 */
static bool
possibly_become_daemon (int level, const struct options* options, const bool first_time)
{
  bool ret = false;
  if (first_time && options->daemon)
    {
      ASSERT (!options->inetd);
      if (level == DAEMONIZATION_LEVEL)
	{
	  if (daemon (options->cd_dir != NULL, 0) < 0)
	    msg (M_ERR, "daemon() failed");
	  ret = true;
	}
    }
  return ret;
}

/* Handle signals */

static volatile int signal_received = 0;

#ifdef HAVE_SIGNAL_H

/* normal signal handler, when we are in event loop */
static void
signal_handler (int signum)
{
  signal_received = signum;
  signal (signum, signal_handler);
}

/* temporary signal handler, before we are fully initialized */
static void
signal_handler_exit (int signum)
{
  msg (M_FATAL | M_NOLOCK, "Signal %d received during initialization, exiting", signum);
}

#endif /* HAVE_SIGNAL_H */

/*
 * For debugging, dump a packet in
 * nominally human-readable form.
 */
#if defined(USE_CRYPTO) && defined(USE_SSL)
#define TLS_MODE (tls_multi != NULL)
#define PROTO_DUMP_FLAGS (check_debug_level (D_UDP_RW_VERBOSE) ? (PD_SHOW_DATA|PD_VERBOSE) : 0)
#define PROTO_DUMP(buf) protocol_dump(buf, \
				      PROTO_DUMP_FLAGS | \
				      (tls_multi ? PD_TLS : 0) | \
				      (options->tls_auth_file ? ks->key_type.hmac_length : 0) \
				      )
#else
#define TLS_MODE (false)
#define PROTO_DUMP(buf) format_hex (BPTR (buf), BLEN (buf), 80)
#endif

#ifdef USE_CRYPTO
#define MD5SUM(buf, len) md5sum(buf, len)
#else
#define MD5SUM(buf, len) "[unavailable]"
#endif

#if defined(USE_PTHREAD) && defined(USE_CRYPTO)
static void *test_crypto_thread (void *arg);
#endif

/*
 * Our global key schedules, packaged thusly
 * to facilitate --persist-key.
 */

struct key_schedule
{
#ifdef USE_CRYPTO
  /* which cipher, HMAC digest, and key sizes are we using? */
  struct key_type   key_type;

  /* pre-shared static key, read from a file */
  struct key_ctx_bi static_key;

#ifdef USE_SSL
  /* our global SSL context */
  SSL_CTX           *ssl_ctx;

  /* optional authentication HMAC key for TLS control channel */
  struct key_ctx_bi tls_auth_key;

#endif /* USE_SSL */
#else /* USE_CRYPTO */
  int dummy;
#endif /* USE_CRYPTO */
};

static void
key_schedule_free(struct key_schedule* ks)
{
#ifdef USE_CRYPTO
  free_key_ctx_bi (&ks->static_key);
#ifdef USE_SSL
  if (ks->ssl_ctx)
    SSL_CTX_free (ks->ssl_ctx);
  free_key_ctx_bi (&ks->tls_auth_key);
#endif /* USE_SSL */
#endif /* USE_CRYPTO */
  CLEAR (*ks);
}

/*
 * struct packet_id_persist should be empty if we are not
 * building with crypto.
 */
#ifndef PACKET_ID_H
struct packet_id_persist { int dummy; };
static inline void packet_id_persist_init (struct packet_id_persist *p) {}
#endif

/*
 * Finalize MTU parameters based on command line or config file options.
 */
static void
frame_finalize_options (struct frame *frame, const struct options *options)
{

  frame_finalize (frame,
		  options->udp_mtu_defined,
		  options->udp_mtu,
		  options->tun_mtu_defined,
		  options->tun_mtu,
#ifdef FRAGMENT_ENABLE
		  options->mtu_min_defined,
		  options->mtu_min,
		  options->mtu_max_defined,
		  options->mtu_max
#else
		  false,
		  0,
		  false,
		  0
#endif
		  );
}

/*
 * Do the work.  Initialize and enter main event loop.
 * Called after command line has been parsed.
 *
 * first_time is true during our first call -- we may
 * be called multiple times due to SIGHUP or SIGUSR1.
 */
static int
openvpn (const struct options *options,
	 struct udp_socket_addr *udp_socket_addr,
	 struct tuntap *tuntap,
	 struct key_schedule *ks,
	 struct packet_id_persist *pid_persist,
	 bool first_time)
{
  /*
   * Initialize garbage collection level.
   * When we pop the level at the end
   * of the routine, everything we
   * allocated with gc_malloc at our level
   * or recursively lower levels will
   * automatically be freed.
   */
  const int gc_level = gc_new_level ();

  /* used by select() */
  int fm;

#if PASSTOS_CAPABILITY
  /* used to get/set TOS. */
  struct iphdr *iph;
  unsigned char ptos;
  bool ptos_defined = false;
#endif

  /* declare various buffers */
  struct buffer to_tun = clear_buf ();
  struct buffer to_udp = clear_buf ();
  struct buffer buf = clear_buf ();
  struct buffer ping_buf = clear_buf ();
  struct buffer nullbuf = clear_buf ();

  /* tells us to free to_udp buffer after it has been written to UDP port */
  bool free_to_udp;

  /* used by select() */
  fd_set reads, writes;

  struct udp_socket udp_socket;   /* socket used for UDP connection to remote */
  struct sockaddr_in to_udp_addr; /* IP address of remote */

  int max_rw_size_udp = 0;        /* max size of packet we can send to remote */

  /* MTU frame parameters */
  struct frame frame;

#ifdef FRAGMENT_ENABLE
  /* Object to handle advanced MTU negotiation and datagram fragmentation */
  struct fragment_master *fragment = NULL;
  struct frame frame_fragment;
  struct frame frame_fragment_omit;
#endif

  /* Always set to current time. */
  time_t current;

#ifdef HAVE_GETTIMEOFDAY
  /*
   * Traffic shaper object.
   */
  struct shaper shaper;
#endif

  /*
   * Statistics
   */
  counter_type tun_read_bytes = 0;
  counter_type tun_write_bytes = 0;
  counter_type udp_read_bytes = 0;
  counter_type udp_write_bytes = 0;

  /*
   * Timer objects for ping and inactivity
   * timeout features.
   */
  struct event_timeout inactivity_interval;
  struct event_timeout ping_send_interval;
  struct event_timeout ping_rec_interval;

  /*
   * This random string identifies an OpenVPN ping packet.
   * It should be of sufficient length and randomness
   * so as not to collide with other tunnel data.
   */
  static const uint8_t ping_string[] = {
    0x2a, 0x18, 0x7b, 0xf3, 0x64, 0x1e, 0xb4, 0xcb,
    0x07, 0xed, 0x2d, 0x0a, 0x98, 0x1f, 0xc7, 0x48
  };

#ifdef USE_CRYPTO

  /*
   * TLS-mode crypto objects.
   */
#ifdef USE_SSL

  /* master OpenVPN SSL/TLS object */
  struct tls_multi *tls_multi = NULL;

  /* an options string that must match on TLS client and server */
  char *data_channel_options = NULL;

#ifdef USE_PTHREAD

  /* object containing TLS thread state */
  struct thread_parms thread_parms;

  /* object sent to us by TLS thread */
  struct tt_ret tt_ret;

#else

  /* used to optimize calls to tls_multi_process
     in single-threaded mode */
  struct interval tmp_int;

#endif
#endif

  /* workspace buffers used by crypto routines */
  struct buffer encrypt_buf = clear_buf ();
  struct buffer decrypt_buf = clear_buf ();

  /* passed to encrypt or decrypt, contains all
     crypto-related command line options related
     to data channel encryption/decryption */
  struct crypto_options crypto_options;

  /* used to keep track of data channel packet sequence numbers */
  struct packet_id packet_id;

  /* our residual iv from all encrypts */
  uint8_t iv[EVP_MAX_IV_LENGTH];
#endif

  /*
   * LZO compression library objects.
   */
#ifdef USE_LZO
  struct buffer lzo_compress_buf = clear_buf ();
  struct buffer lzo_decompress_buf = clear_buf ();
  struct lzo_compress_workspace lzo_compwork;
#endif

  /*
   * Buffers used to read from TUN device
   * and UDP port.
   */
  struct buffer read_udp_buf = clear_buf ();
  struct buffer read_tun_buf = clear_buf ();

  /*
   * IPv4 TUN device?
   */
  bool ipv4_tun = (!options->tun_ipv6 && is_dev_type (options->dev, options->dev_type, "tun"));
  const bool ipv6_udp_transport = false; /* we don't support IPv6 UDP sockets yet */

  /* workspace for get_pid_file/write_pid */
  struct pid_state pid_state;

  /* workspace for --user/--group */
  struct user_state user_state;
  struct group_state group_state;

  /* temporary variable */
  bool did_we_daemonize;

#ifdef HAVE_SIGNAL_H
  /*
   * Special handling if signal arrives before
   * we are properly initialized.
   */
  signal (SIGINT, signal_handler_exit);
  signal (SIGTERM, signal_handler_exit);
  signal (SIGHUP, SIG_IGN);
  signal (SIGUSR1, SIG_IGN);
  signal (SIGUSR2, SIG_IGN);
  signal (SIGPIPE, SIG_IGN);
#endif /* HAVE_SIGNAL_H */

  msg (M_INFO, "%s", title_string);

  CLEAR (udp_socket);
  CLEAR (frame);
#ifdef FRAGMENT_ENABLE
  CLEAR (frame_fragment_omit);
#endif

  /* should we disable paging? */
  if (first_time && options->mlock)
    do_mlockall (true);

  /*
   * Initialize advanced MTU negotiation and datagram fragmentation
   */
#ifdef FRAGMENT_ENABLE
  if (options->mtu_dynamic)
    fragment = fragment_init (&frame);
#endif

#ifdef USE_CRYPTO
  /* load a persisted packet-id for cross-session replay-protection */
  if (options->packet_id_file)
    packet_id_persist_load (pid_persist, options->packet_id_file);

  if (!options->test_crypto)
#endif
    /* open the UDP socket */
    udp_socket_init (&udp_socket, options->local, options->remote,
		     options->local_port, options->remote_port,
		     options->bind_local, options->remote_float,
		     options->inetd,
		     udp_socket_addr, options->ipchange,
		     options->resolve_retry_seconds,
		     options->mtu_discover_type);

#ifdef USE_CRYPTO

  /* Initialize crypto options */

  CLEAR (crypto_options);
  CLEAR (packet_id);
  CLEAR (iv);

  /* Start with a random IV and carry forward the residuals */
  if (options->iv)
    {
      randomize_iv (iv);
      crypto_options.iv = iv;
    }

  if (options->shared_secret_file)
    {
      /*
       * Static Key Mode (using a pre-shared key)
       */

      /* Initialize packet ID tracking */
      if (options->packet_id)
	{
	  crypto_options.packet_id = &packet_id;
	  crypto_options.pid_persist = pid_persist;
	  crypto_options.packet_id_long_form = true;
	  packet_id_persist_load_obj (pid_persist, crypto_options.packet_id);
	}

      if (!key_ctx_bi_defined (&ks->static_key))
	{
	  struct key key;

	  /* Get cipher & hash algorithms */
	  init_key_type (&ks->key_type, options->ciphername,
			 options->ciphername_defined, options->authname,
			 options->authname_defined, options->keysize,
			 options->test_crypto);

	  /* Read cipher and hmac keys from shared secret file */
	  read_key_file (&key, options->shared_secret_file);

	  /* Fix parity for DES keys and make sure not a weak key */
	  fixup_key (&key, &ks->key_type);
	  if (!check_key (&key, &ks->key_type)) /* This should be a very improbable failure */
	    msg (M_FATAL, "Key in %s is bad.  Try making a new key with --genkey.",
		 options->shared_secret_file);

	  /* Init cipher & hmac */
	  init_key_ctx (&ks->static_key.encrypt, &key, &ks->key_type, DO_ENCRYPT, "Static Encrypt");
	  init_key_ctx (&ks->static_key.decrypt, &key, &ks->key_type, DO_DECRYPT, "Static Decrypt");

	  /* Erase the key */
	  CLEAR (key);
	}
      else
	{
	  msg (M_INFO, "Re-using pre-shared static key");
	}

      /* Get key schedule */
      crypto_options.key_ctx_bi = &ks->static_key;

      /* Compute MTU parameters */
      crypto_adjust_frame_parameters(&frame,
				     &ks->key_type,
				     options->ciphername_defined,
				     options->iv,
				     options->packet_id,
				     true);

      /* Sanity check on IV, sequence number, and cipher mode options */
      check_replay_iv_consistency(&ks->key_type, options->packet_id, options->iv);

      /*
       * Test-crypto is a debugging tool
       * that basically does a loopback test
       * on the crypto subsystem.
       */
      if (options->test_crypto)
	{
#ifdef USE_PTHREAD
	  if (first_time)
	    {
	      thread_init();
	      work_thread_create(test_crypto_thread, (void*) options);
	    }
#endif
	  frame_finalize_options (&frame, options);

	  test_crypto (&crypto_options, &frame);
	  key_schedule_free (ks);
	  signal_received = 0;
#ifdef USE_PTHREAD
	  if (first_time)
	    work_thread_join ();
#endif
	  goto done;
	}
    }
#ifdef USE_SSL
  else if (options->tls_server || options->tls_client)
    {
      /*
       * TLS-based dynamic key exchange mode
       */
      struct tls_options to;
      bool packet_id_long_form;

      ASSERT (!options->test_crypto);

      /* Make sure we are either a TLS client or server but not both */
      ASSERT (options->tls_server == !options->tls_client);

      /* Let user specify a script to verify the incoming certificate */
      tls_set_verify_command (options->tls_verify);

      if (!ks->ssl_ctx)
	{
	  /*
	   * Initialize the OpenSSL library's global
	   * SSL context.
	   */
	  ks->ssl_ctx = init_ssl (options->tls_server,
				  options->ca_file,
				  options->dh_file,
				  options->cert_file,
				  options->priv_key_file,
				  options->cipher_list);

	  /* Get cipher & hash algorithms */
	  init_key_type (&ks->key_type, options->ciphername,
			 options->ciphername_defined, options->authname,
			 options->authname_defined, options->keysize,
			 true);


	  /* TLS handshake authentication (--tls-auth) */
	  if (options->tls_auth_file)
	    get_tls_handshake_key (&ks->key_type, &ks->tls_auth_key, options->tls_auth_file);
	}
      else
	{
	  msg (M_INFO, "Re-using SSL/TLS context");
	}

      /* Sanity check on IV, sequence number, and cipher mode options */
      check_replay_iv_consistency(&ks->key_type, options->packet_id, options->iv);

      /* In short form, unique datagram identifier is 32 bits, in long form 64 bits */
      packet_id_long_form = cfb_ofb_mode (&ks->key_type);

      /* Compute MTU parameters */
      crypto_adjust_frame_parameters(&frame,
				     &ks->key_type,
				     options->ciphername_defined,
				     options->iv,
				     options->packet_id,
				     packet_id_long_form);
      tls_adjust_frame_parameters(&frame);

      /* Set all command-line TLS-related options */
      CLEAR (to);
      to.ssl_ctx = ks->ssl_ctx;
      to.key_type = ks->key_type;
      to.server = options->tls_server;
      to.options = data_channel_options = options_string (options);
      to.packet_id = options->packet_id;
      to.packet_id_long_form = packet_id_long_form;
      to.transition_window = options->transition_window;
      to.handshake_window = options->handshake_window;
      to.packet_timeout = options->tls_timeout;
      to.renegotiate_bytes = options->renegotiate_bytes;
      to.renegotiate_packets = options->renegotiate_packets;
      to.renegotiate_seconds = options->renegotiate_seconds;
      to.single_session = options->single_session;
      to.disable_occ = options->disable_occ;

      /* TLS handshake authentication (--tls-auth) */
      if (options->tls_auth_file)
	{
	  to.tls_auth_key = ks->tls_auth_key;
	  to.tls_auth.pid_persist = pid_persist;
	  to.tls_auth.packet_id_long_form = true;
	  crypto_adjust_frame_parameters(&to.frame,
					 &ks->key_type,
					 false,
					 false,
					 true,
					 true);
	}

      /*
       * Initialize OpenVPN's master TLS-mode object.
       */
      tls_multi = tls_multi_init (&to, &udp_socket);
    }
#endif
  else
    {
      /*
       * No encryption or authentication.
       */
      ASSERT (!options->test_crypto);
      free_key_ctx_bi (&ks->static_key);
      crypto_options.key_ctx_bi = &ks->static_key;
      msg (M_WARN,
	   "******* WARNING *******: all encryption and authentication features disabled -- all data will be tunnelled as cleartext");
    }

#else /* USE_CRYPTO */

  msg (M_WARN,
       "******* WARNING *******: OpenVPN built without OpenSSL -- encryption and authentication features disabled -- all data will be tunnelled as cleartext");

#endif /* USE_CRYPTO */

#ifdef USE_LZO
  /*
   * Initialize LZO compression library.
   */
  if (options->comp_lzo)
    {
      lzo_compress_init (&lzo_compwork, options->comp_lzo_adaptive);
      lzo_adjust_frame_parameters (&frame);
#ifdef FRAGMENT_ENABLE
      lzo_adjust_frame_parameters (&frame_fragment_omit); /* omit LZO frame delta from final frame_fragment */
#endif
    }
#endif

  /*
   * Adjust frame size based on the --tun-mtu-extra parameter.
   */
  tun_adjust_frame_parameters (&frame, options->tun_mtu_extra);

  /*
   * Fill in the blanks in the frame parameters structure,
   * make sure values are rational, etc.
   */
  frame_finalize_options (&frame, options);

  /*
   * Set frame parameter for fragment code.  This is necessary because
   * the fragmentation code deals with payloads which have already been
   * passed through the compression code.
   */
#ifdef FRAGMENT_ENABLE
  frame_fragment = frame;
  frame_subtract_extra (&frame_fragment, &frame_fragment_omit);
  frame_dynamic_finalize (&frame_fragment);
#endif

  max_rw_size_udp = MAX_RW_SIZE_UDP (&frame);
  frame_print (&frame, D_MTU_INFO, "Data Channel MTU parms");
#ifdef FRAGMENT_ENABLE
  if (fragment)
    frame_print (&frame_fragment, D_FRAG_DEBUG, "Fragmentation MTU parms");
#endif

#if defined(USE_CRYPTO) && defined(USE_SSL)
  if (tls_multi)
    {
      int size;

      tls_multi_init_finalize (tls_multi, &frame);
      size = MAX_RW_SIZE_UDP (&tls_multi->opt.frame);
      if (size > max_rw_size_udp)
	max_rw_size_udp = size;
      frame_print (&tls_multi->opt.frame, D_MTU_INFO, "Control Channel MTU parms");
    }
#endif

  /*
   * Now that we know all frame parameters, initialize
   * our buffers.
   */

  read_udp_buf = alloc_buf (BUF_SIZE (&frame));
  read_tun_buf = alloc_buf (BUF_SIZE (&frame));

#ifdef USE_CRYPTO
  encrypt_buf = alloc_buf (BUF_SIZE (&frame));
  decrypt_buf = alloc_buf (BUF_SIZE (&frame));
#endif

#ifdef USE_LZO
  if (options->comp_lzo)
    {
      lzo_compress_buf = alloc_buf (BUF_SIZE (&frame));
      lzo_decompress_buf = alloc_buf (BUF_SIZE (&frame));
    }
#endif

#ifdef FRAGMENT_ENABLE
  /* fragmenting code has buffers to initialize once frame parameters are known */
  if (fragment)
    fragment_frame_init (fragment, &frame_fragment, (options->mtu_icmp && ipv4_tun));
#endif

  if (!tuntap_defined (tuntap))
    {
      /* do ifconfig */
      if (ifconfig_order() == IFCONFIG_BEFORE_TUN_OPEN)
	do_ifconfig (options->dev, options->dev_type,
		     options->ifconfig_local, options->ifconfig_remote,
		     TUN_MTU_SIZE (&frame));

      /* open the tun device */
      open_tun (options->dev, options->dev_type, options->dev_node, options->tun_ipv6, tuntap);

      /* do ifconfig */  
      if (ifconfig_order() == IFCONFIG_AFTER_TUN_OPEN)
	do_ifconfig (tuntap->actual, options->dev_type,
		     options->ifconfig_local, options->ifconfig_remote,
		     TUN_MTU_SIZE (&frame));

      /* run the up script */
      run_script (options->up_script, tuntap->actual, TUN_MTU_SIZE (&frame),
		  max_rw_size_udp, options->ifconfig_local, options->ifconfig_remote);
    }
  else
    {
      msg (M_INFO, "Preserving previous TUN/TAP instance: %s", tuntap->actual);
    }

#ifdef HAVE_GETTIMEOFDAY
  /* initialize traffic shaper (i.e. transmit bandwidth limiter) */
  if (options->shaper)
    {
      shaper_init (&shaper, options->shaper);
      shaper_msg (&shaper);
    }
#endif

  if (first_time)
    {
      /* get user and/or group that we want to setuid/setgid to */
      get_group (options->groupname, &group_state);
      get_user (options->username, &user_state);

      /* get --writepid file descriptor */
      get_pid_file (options->writepid, &pid_state);

      /* chroot if requested */
      do_chroot (options->chroot_dir);
    }

  /* become a daemon if --daemon */
  did_we_daemonize = possibly_become_daemon (1, options, first_time);

#ifdef HAVE_SIGNAL_H
  /* catch signals */
  signal (SIGINT, signal_handler);
  signal (SIGTERM, signal_handler);
  signal (SIGHUP, signal_handler);
  signal (SIGUSR1, signal_handler);
  signal (SIGUSR2, signal_handler);
#endif /* HAVE_SIGNAL_H */

  if (first_time)
    {
      /* should we disable paging? */
      if (options->mlock && did_we_daemonize)
	do_mlockall (true); /* call again in case we daemonized */

      /* set user and/or group that we want to setuid/setgid to */
      set_group (&group_state);
      set_user (&user_state);

      /* save process ID in a file */
      write_pid (&pid_state);

      /* initialize threading if pthread configure option enabled */
      thread_init();
    }

  /* start the TLS thread */
#if defined(USE_CRYPTO) && defined(USE_SSL) && defined(USE_PTHREAD)
  if (tls_multi)
    tls_thread_create (&thread_parms, tls_multi, &udp_socket,
		       options->nice_work, options->mlock);
#endif

  /* change scheduling priority if requested */
  if (first_time)
    set_nice (options->nice);

  /*
   * MAIN EVENT LOOP
   *
   * Pipe UDP -> tun and tun -> UDP using nonblocked i/o.
   *
   * If tls_multi is defined, multiplex a TLS
   * control channel over the UDP connection which
   * will be used for secure key exchange with our peer.
   *
   */

  /* calculate max file handle + 1 for select */
  fm = udp_socket.sd;
  if (tuntap->fd > fm)
    fm = tuntap->fd;

  current = time (NULL);

  /* initialize inactivity timeout */
  if (options->inactivity_timeout)
    event_timeout_init (&inactivity_interval, current, options->inactivity_timeout);

  /* initialize pings */

  if (options->ping_send_timeout)
    {
      ping_buf = alloc_buf (BUF_SIZE (&frame));
      event_timeout_init (&ping_send_interval, 0, options->ping_send_timeout);
    }

  if (options->ping_rec_timeout)
    event_timeout_init (&ping_rec_interval, current, options->ping_rec_timeout);

#if defined(USE_CRYPTO) && defined(USE_SSL)
#ifdef USE_PTHREAD
  if (tls_multi && TLS_THREAD_SOCKET (&thread_parms) > fm)
    fm = TLS_THREAD_SOCKET (&thread_parms);
#else
  /* initialize tmp_int optimization that limits the number of times we call
     tls_multi_process in the main event loop */
  interval_init (&tmp_int, TLS_MULTI_HORIZON, TLS_MULTI_REFRESH);
#endif
#endif

  /* select() needs this */
  ++fm;

  /* this flag is true for buffers coming from the TLS background thread */
  free_to_udp = false;

  while (true)
    {
      int stat = 0;
      struct timeval *tv = NULL;
      struct timeval timeval;

      /* initialize select() timeout */
      timeval.tv_sec = 0;
      timeval.tv_usec = 0;

#ifdef USE_CRYPTO
      /* flush current packet-id to file once per 60
	 seconds if --replay-persist was specified */
      packet_id_persist_flush (pid_persist, current, 60);
#endif

#if defined(USE_CRYPTO) && defined(USE_SSL) && !defined(USE_PTHREAD)
      /*
       * In TLS mode, let TLS level respond to any control-channel packets which were
       * received, or prepare any packets for transmission.
       *
       * tmp_int is purely an optimization that allows us to call tls_multi_process
       * less frequently when there's not much traffic on the control-channel.
       *
       */
      if (tls_multi)
	{
	  interval_t wakeup = 0;

	  if (interval_test (&tmp_int, current))
	    {
	      if (tls_multi_process (tls_multi, &to_udp, &to_udp_addr, &udp_socket, &wakeup, current))
		interval_action (&tmp_int, current);

	      interval_future_trigger (&tmp_int, wakeup, current);
	      free_to_udp = false;
	    }

	  interval_schedule_wakeup (&tmp_int, current, &wakeup);

	  if (wakeup)
	    {
	      timeval.tv_sec = wakeup;
	      timeval.tv_usec = 0;
	      tv = &timeval;
	    }
	}
#endif

      current = time (NULL);

      /*
       * Should we exit due to inactivity timeout?
       */
      if (options->inactivity_timeout)
	{
	  if (event_timeout_trigger (&inactivity_interval, current)) 
	    {
	      msg (M_INFO, "Inactivity timeout (--inactive), exiting");
	      signal_received = 0;
	      break;
	    }
	  event_timeout_wakeup (&inactivity_interval, current, &timeval);
	  tv = &timeval;
	}

      /*
       * Should we exit or restart due to ping (or other authenticated packet)
       * not received in n seconds?
       */
      if (options->ping_rec_timeout &&
	  (!options->ping_timer_remote || addr_defined (&udp_socket_addr->actual)))
	{
	  if (event_timeout_trigger (&ping_rec_interval, current)) 
	    {
	      switch (options->ping_rec_timeout_action)
		{
		case PING_EXIT:
		  msg (M_INFO, "Inactivity timeout (--ping-exit), exiting");
		  signal_received = 0;
		  break;
		case PING_RESTART:
		  msg (M_INFO, "Inactivity timeout (--ping-restart), restarting");
		  signal_received = SIGUSR1;
		  break;
		default:
		  ASSERT (0);
		}
	      break;
	    }
	  event_timeout_wakeup (&ping_rec_interval, current, &timeval);
	  tv = &timeval;
	}

#ifdef FRAGMENT_ENABLE
      /*
       * Should we deliver a datagram fragment to remote?
       */
      if (fragment)
	{
	  /* OS MTU Hint? */
	  if (udp_socket.mtu_changed && ipv4_tun)
	    {
	      frame_adjust_path_mtu (&frame_fragment, udp_socket.mtu, ipv6_udp_transport);
	      udp_socket.mtu_changed = false;
	    }
	  if (!to_udp.len
	      && fragment_outgoing_defined (fragment)
	      && fragment_ready_to_send (fragment, &buf, &frame_fragment))
	    {
#ifdef USE_CRYPTO
#ifdef USE_SSL
	      /*
	       * If TLS mode, get the key we will use to encrypt
	       * the packet.
	       */
	      mutex_lock (L_TLS);
	      if (tls_multi)
		tls_pre_encrypt (tls_multi, &buf, &crypto_options);
#endif
	      /*
	       * Encrypt the packet and write an optional
	       * HMAC authentication record.
	       */
	      openvpn_encrypt (&buf, encrypt_buf, &crypto_options, &frame, current);
#endif
	      /*
	       * Get the address we will be sending the packet to.
	       */
	      udp_socket_get_outgoing_addr (&buf, &udp_socket,
					    &to_udp_addr);
#ifdef USE_CRYPTO
#ifdef USE_SSL
	      /*
	       * In TLS mode, prepend the appropriate one-byte opcode
	       * to the packet which identifies it as a data channel
	       * packet and gives the low-permutation version of
	       * the key-id to the recipient so it knows which
	       * decrypt key to use.
	       */
	      if (tls_multi)
		tls_post_encrypt (tls_multi, &buf);
	      mutex_unlock (L_TLS);
#endif
#endif
	      to_udp = buf;
	      free_to_udp = false;
	    }
	  if (!to_tun.len && fragment_icmp (fragment, &buf))
	    {
	      to_tun = buf;
	    }
	  fragment_housekeeping (fragment, &frame_fragment, current, &timeval);
	  tv = &timeval;
	}
#endif /* FRAGMENT_ENABLE */

      /*
       * Should we ping the remote?
       */
      if (options->ping_send_timeout)
	{
	  if (!to_udp.len)
	    {
	      if (event_timeout_trigger (&ping_send_interval, current))
		{
		  buf = ping_buf;
		  ASSERT (buf_init (&buf, EXTRA_FRAME (&frame)));
		  ASSERT (buf_safe (&buf, MAX_RW_SIZE_TUN (&frame)));
		  ASSERT (buf_write (&buf, ping_string, sizeof (ping_string)));

		  /*
		   * We will treat the ping like any other outgoing packet,
		   * encrypt, authenticate, etc.
		   */
#ifdef USE_LZO
		  if (options->comp_lzo)
		    lzo_compress (&buf, lzo_compress_buf, &lzo_compwork, &frame, current);
#endif
#ifdef FRAGMENT_ENABLE
		  if (fragment)
		    fragment_outgoing (fragment, &buf, &frame_fragment, current);
#endif
#ifdef USE_CRYPTO
#ifdef USE_SSL
		  mutex_lock (L_TLS);
		  if (tls_multi)
		    tls_pre_encrypt (tls_multi, &buf, &crypto_options);
#endif
		  openvpn_encrypt (&buf, encrypt_buf, &crypto_options, &frame, current);
#endif
		  udp_socket_get_outgoing_addr (&buf, &udp_socket,
						&to_udp_addr);
#ifdef USE_CRYPTO
#ifdef USE_SSL
		  if (tls_multi)
		    tls_post_encrypt (tls_multi, &buf);
		  mutex_unlock (L_TLS);
#endif
#endif
		  to_udp = buf;
		  free_to_udp = false;
		  msg (D_PACKET_CONTENT, "SENT PING");
		}
	      event_timeout_wakeup (&ping_send_interval, current, &timeval);
	      tv = &timeval;
	    }
	}

      /* do a quick garbage collect */
      gc_collect (gc_level);

      /*
       * Set up for select call.
       *
       * Decide what kind of events we want to wait for.
       */
      FD_ZERO (&reads);
      FD_ZERO (&writes);

      /*
       * If outgoing data (for UDP port) pending, wait for ready-to-send
       * status from UDP port. Otherwise, wait for incoming data on TUN/TAP device.
       */
      if (to_udp.len > 0)
	{
	  /*
	   * If sending this packet would put us over our traffic shaping
	   * quota, don't send -- instead compute the delay we must wait
	   * until it will be OK to send the packet.
	   */

#ifdef HAVE_GETTIMEOFDAY
	  int delay = 0;

	  /* set traffic shaping delay in microseconds */
	  if (options->shaper)
	    delay = max_int (delay, shaper_delay (&shaper));

	  if (delay >= 1000)
	    {
	      shaper_soonest_event (&timeval, delay);
	      tv = &timeval;
	    }
	  else
	    {
	      FD_SET (udp_socket.sd, &writes);
	    }
#else /* HAVE_GETTIMEOFDAY */
	  FD_SET (udp_socket.sd, &writes);
#endif /* HAVE_GETTIMEOFDAY */
	}
#ifdef FRAGMENT_ENABLE
      else if (!fragment || !fragment_outgoing_defined (fragment))
#else
      else
#endif
	{
	  if (tuntap->fd >= 0)
	    {
	      FD_SET (tuntap->fd, &reads);
	    }
#if defined(USE_CRYPTO) && defined(USE_SSL) && defined(USE_PTHREAD)
	  if (tls_multi)
	    FD_SET (TLS_THREAD_SOCKET (&thread_parms), &reads);
#endif
	}

      /*
       * If outgoing data (for TUN/TAP device) pending, wait for ready-to-send status
       * from device.  Otherwise, wait for incoming data on UDP port.
       */
      if (to_tun.len > 0)
	{
	  if (tuntap->fd >= 0)
	    FD_SET (tuntap->fd, &writes);
	}
      else
	{
	  FD_SET (udp_socket.sd, &reads);
	}

      /*
       * Possible scenarios:
       *  (1) udp port has data available to read
       *  (2) udp port is ready to accept more data to write
       *  (3) tun dev has data available to read
       *  (4) tun dev is ready to accept more data to write
       *  (5) tls background thread has data available to forward to udp port
       *  (6) we received a signal (handler sets signal_received)
       *  (7) timeout (tv) expired (from TLS, shaper, inactivity timeout, or ping timeout)
       */

      /*
       * Wait for something to happen.
       */
      if (!signal_received) {
	msg (D_SELECT, "SELECT %s|%s|%s|%s %d/%d",
	     FD_ISSET (tuntap->fd, &reads) ?     "TR" : "tr", 
	     FD_ISSET (tuntap->fd, &writes) ?    "TW" : "tw", 
	     FD_ISSET (udp_socket.sd, &reads) ?  "UR" : "ur",
	     FD_ISSET (udp_socket.sd, &writes) ? "UW" : "uw",
	     tv ? (int)tv->tv_sec : -1,
	     tv ? (int)tv->tv_usec : -1
	     );
	stat = select (fm, &reads, &writes, NULL, tv);
	check_status (stat, "select", NULL);
      }

      /* current should always be a reasonably up-to-date timestamp */
      current = time (NULL);

#ifdef HAVE_SIGNAL_H
      /*
       * Did we get a signal before or while we were waiting
       * in select() ?
       */
      if (signal_received)
	{
	  if (signal_received == SIGUSR2)
	    {
	      msg (M_INFO, "Current OpenVPN Statistics:");
	      msg (M_INFO, " TUN/TAP read bytes:   " counter_format, tun_read_bytes);
	      msg (M_INFO, " TUN/TAP write bytes:  " counter_format, tun_write_bytes);
	      msg (M_INFO, " UDP read bytes:       " counter_format, udp_read_bytes);
	      msg (M_INFO, " UDP write bytes:      " counter_format, udp_write_bytes);
#ifdef USE_LZO
	      if (options->comp_lzo)
		  lzo_print_stats (&lzo_compwork);		  
#endif
	      signal_received = 0;
	      continue;
	    }

	  /* for all other signals (INT, TERM, HUP, USR1) we break */
	  switch (signal_received)
	    {
	    case SIGINT:
	      msg (M_INFO, "SIGINT received, exiting");
	      break;
	    case SIGTERM:
	      msg (M_INFO, "SIGTERM received, exiting");
	      break;
	    case SIGHUP:
	      msg (M_INFO, "SIGHUP received, restarting");
	      break;
	    case SIGUSR1:
	      msg (M_INFO, "SIGUSR1 received, restarting");
	      break;
	    default:
	      msg (M_INFO, "Unknown signal %d received", signal_received);
	      break;
	    }
	  break;
	}
#endif /* HAVE_SIGNAL_H */

      if (!stat) /* timeout? */
	continue;

      if (stat > 0)
	{
	  /* Incoming data on UDP port */
	  if (FD_ISSET (udp_socket.sd, &reads))
	    {
	      /*
	       * Set up for recvfrom call to read datagram
	       * sent to our UDP port from any source address.
	       */
	      struct sockaddr_in from;
	      socklen_t fromlen = sizeof (from);
	      ASSERT (!to_tun.len);
	      buf = read_udp_buf;
	      ASSERT (buf_init (&buf, EXTRA_FRAME (&frame)));
	      ASSERT (buf_safe (&buf, max_rw_size_udp));
	      fromlen = sizeof (from);
	      buf.len = recvfrom (udp_socket.sd, BPTR (&buf), max_rw_size_udp, 0,
				  (struct sockaddr *) &from, &fromlen);
	      ASSERT (fromlen == sizeof (from));
	      if (buf.len > 0)
		udp_read_bytes += buf.len;

	      /* check recvfrom status */
	      check_status (buf.len, "read from UDP", &udp_socket);

	      /* take action to corrupt packet if we are in gremlin test mode */
	      if (options->gremlin) {
		if (!ask_gremlin())
		  buf.len = 0;
		corrupt_gremlin(&buf);
	      }

	      /* log incoming packet */
#ifdef LOG_RW
	      if (check_debug_level (D_LOG_RW) && !check_debug_level (D_LOG_RW + 1))
		fprintf (stderr, "R");
#endif
	      msg (D_UDP_RW, "UDP READ [%d] from %s: %s",
		   BLEN (&buf), print_sockaddr (&from), PROTO_DUMP (&buf));

	      /*
	       * Good, non-zero length packet received.
	       * Commence multi-stage processing of packet,
	       * such as authenticate, decrypt, decompress.
	       * If any stage fails, it sets buf.len to 0,
	       * telling downstream stages to ignore the packet.
	       */
	      if (buf.len > 0)
		{
		  udp_socket_incoming_addr (&buf, &udp_socket, &from);
#ifdef USE_CRYPTO
#ifdef USE_SSL
		  mutex_lock (L_TLS);
		  if (tls_multi)
		    {
		      /*
		       * If tls_pre_decrypt returns true, it means the incoming
		       * packet was a good TLS control channel packet.  If so, TLS code
		       * will deal with the packet and set buf.len to 0 so downstream
		       * stages ignore it.
		       *
		       * If the packet is a data channel packet, tls_pre_decrypt
		       * will load crypto_options with the correct encryption key
		       * and return false.
		       */
		      if (tls_pre_decrypt (tls_multi, &from, &buf, &crypto_options, current))
			{
#ifdef USE_PTHREAD
			  /* tell TLS thread a packet is waiting */
			  if (tls_thread_process (&thread_parms) == -1)
			    {
			      msg (M_WARN, "TLS thread is not responding, exiting (1)");
			      signal_received = 0;
			      mutex_unlock (L_TLS);
			      break;
			    }
#else
			  interval_action (&tmp_int, current);
#endif /* USE_PTHREAD */
			  /* reset packet received timer if TLS packet */
			  if (options->ping_rec_timeout)
			    event_timeout_reset (&ping_rec_interval, current);
			}
		    }
#endif /* USE_SSL */
		  /* authenticate and decrypt the incoming packet */
		  openvpn_decrypt (&buf, decrypt_buf, &crypto_options, &frame, current);
#ifdef USE_SSL
		  mutex_unlock (L_TLS);
#endif /* USE_SSL */
#endif /* USE_CRYPTO */
#ifdef FRAGMENT_ENABLE
		  if (fragment)
		    fragment_incoming (fragment, &buf, &frame_fragment, current);
#endif
#ifdef USE_LZO
		  /* decompress the incoming packet */
		  if (options->comp_lzo)
		    lzo_decompress (&buf, lzo_decompress_buf, &lzo_compwork, &frame);
#endif
		  /*
		   * Set our "official" outgoing address, since
		   * if buf.len is non-zero, we know the packet
		   * authenticated.  In TLS mode we do nothing
		   * because TLS mode takes care of source address
		   * authentication.
		   *
		   * Also, update the persisted version of our packet-id.
		   */
		  if (!TLS_MODE)
		    udp_socket_set_outgoing_addr (&buf, &udp_socket, &from);

		  /* reset packet received timer */
		  if (options->ping_rec_timeout && buf.len > 0)
		    event_timeout_reset (&ping_rec_interval, current);

		  /* Did we just receive an openvpn ping packet? */
		  if (buf_string_match (&buf, ping_string, sizeof (ping_string)))
		    {
		      msg (D_PACKET_CONTENT, "RECEIVED PING");
		      buf.len = 0; /* drop it */
		    }

		  to_tun = buf;
		}
	      else
		{
		  to_tun = nullbuf;
		}
	    }

#if defined(USE_CRYPTO) && defined(USE_SSL) && defined(USE_PTHREAD)
	  /* Incoming data from TLS background thread */
	  else if (tls_multi && FD_ISSET (TLS_THREAD_SOCKET (&thread_parms), &reads))
	    {
	      int s;
	      ASSERT (!to_udp.len);

	      s = tls_thread_rec_buf (&thread_parms, &tt_ret, true);
	      if (s == 1)
		{
		  /*
		   * TLS background thread has a control channel
		   * packet to send to remote.
		   */
		  to_udp = tt_ret.to_udp;
		  to_udp_addr = tt_ret.to_udp_addr;
		
		  /* tell UDP packet writer to free buffer after write */
		  free_to_udp = true;
		}

	      /* remote died? */
	      else if (s == -1)
		{
		  msg (M_WARN, "TLS thread is not responding, exiting (2)");
		  signal_received = 0;
		  break;
		}
	    }
#endif

	  /* Incoming data on TUN device */
	  else if (tuntap->fd >= 0 && FD_ISSET (tuntap->fd, &reads))
	    {
	      /*
	       * Setup for read() call on TUN/TAP device.
	       */
	      ASSERT (!to_udp.len);
	      buf = read_tun_buf;
	      ASSERT (buf_init (&buf, EXTRA_FRAME (&frame)));
	      ASSERT (buf_safe (&buf, MAX_RW_SIZE_TUN (&frame)));
	      buf.len = read_tun (tuntap, BPTR (&buf), MAX_RW_SIZE_TUN (&frame));
	      if (buf.len > 0)
		tun_read_bytes += buf.len;

	      /* Check the status return from read() */
	      check_status (buf.len, "read from TUN/TAP", NULL);

	      /* show packet content */
	      msg (D_TUN_RW, "TUN READ [%d]: %s md5=%s",
		   BLEN (&buf),
		   format_hex (BPTR (&buf), BLEN (&buf), 80),
		   MD5SUM (BPTR (&buf), BLEN (&buf)));

#ifdef FRAGMENT_ENABLE
	      /* if packet is too big, we might want to bounce back a "fragmentation
		 needed but DF set ICMP message */
	      if (fragment)
		fragment_check_fragmentability (fragment, &frame_fragment, &buf);
#endif

	      if (buf.len > 0)
		{
#if PASSTOS_CAPABILITY
		  if (options->passtos)
		    {
		      /* Get the TOS before compression/encryption. */
		      iph = (struct iphdr*) BPTR (&buf);

		      /* Check that it's an IPv4 packet. */
		      if (iph->version == 0x04)
			{
			  ptos = iph->tos;
			  ptos_defined = true;
			}
		    }
#endif

#ifdef USE_LZO
		  /* Compress the packet. */
		  if (options->comp_lzo)
		    lzo_compress (&buf, lzo_compress_buf, &lzo_compwork, &frame, current);
#endif
#ifdef FRAGMENT_ENABLE
		  if (fragment)
		    fragment_outgoing (fragment, &buf, &frame_fragment, current);
#endif
#ifdef USE_CRYPTO
#ifdef USE_SSL
		  /*
		   * If TLS mode, get the key we will use to encrypt
		   * the packet.
		   */
		  mutex_lock (L_TLS);
		  if (tls_multi)
		    tls_pre_encrypt (tls_multi, &buf, &crypto_options);
#endif
		  /*
		   * Encrypt the packet and write an optional
		   * HMAC authentication record.
		   */
		  openvpn_encrypt (&buf, encrypt_buf, &crypto_options, &frame, current);
#endif
		  /*
		   * Get the address we will be sending the packet to.
		   */
		  udp_socket_get_outgoing_addr (&buf, &udp_socket,
						&to_udp_addr);
#ifdef USE_CRYPTO
#ifdef USE_SSL
		  /*
		   * In TLS mode, prepend the appropriate one-byte opcode
		   * to the packet which identifies it as a data channel
		   * packet and gives the low-permutation version of
		   * the key-id to the recipient so it knows which
		   * decrypt key to use.
		   */
		  if (tls_multi)
		    tls_post_encrypt (tls_multi, &buf);
		  mutex_unlock (L_TLS);
#endif
#endif
		  to_udp = buf;
		}
	      else
		{
		  to_udp = nullbuf;
		}
	      free_to_udp = false;
	    }

	  /* TUN device ready to accept write */
	  else if (tuntap->fd >= 0 && FD_ISSET (tuntap->fd, &writes))
	    {
	      /*
	       * Set up for write() call to TUN/TAP
	       * device.
	       */
	      ASSERT (to_tun.len > 0);

	      if (to_tun.len <= MAX_RW_SIZE_TUN(&frame))
		{
		  /*
		   * Write to TUN/TAP device.
		   */
		  int size;

		  msg (D_TUN_RW, "TUN WRITE [%d]: %s md5=%s",
		       BLEN (&to_tun),
		       format_hex (BPTR (&to_tun), BLEN (&to_tun), 80),
		       MD5SUM (BPTR (&to_tun), BLEN (&to_tun)));

		  size = write_tun (tuntap, BPTR (&to_tun), BLEN (&to_tun));
		  if (size > 0)
		    tun_write_bytes += size;
		  check_status (size, "write to TUN/TAP", NULL);

		  /* check written packet size */
		  if (size > 0)
		    {
		      /* Did we write a different size packet than we intended? */
		      if (size != BLEN (&to_tun))
			msg (D_LINK_ERRORS,
			     "TUN/TAP packet was fragmented on write to %s (tried=%d,actual=%d)",
			     tuntap->actual,
			     BLEN (&to_tun),
			     size);
		    }
		}
	      else
		{
		  /*
		   * This should never happen, probably indicates some kind
		   * of MTU mismatch.
		   */
		  msg (D_LINK_ERRORS, "tun packet too large on write (tried=%d,max=%d)",
		       to_tun.len,
		       MAX_RW_SIZE_TUN (&frame));
		}

	      /*
	       * Putting the --inactive timeout reset here, ensures that we will timeout
	       * if the remote goes away, even if we are trying to send data to the
	       * remote and failing.
	       */
	      if (options->inactivity_timeout)
		event_timeout_reset (&inactivity_interval, current);

	      to_tun = nullbuf;
	    }

	  /* UDP port ready to accept write */
	  else if (FD_ISSET (udp_socket.sd, &writes))
	    {
	      if (to_udp.len > 0 && to_udp.len <= max_rw_size_udp)
		{
		  /*
		   * Setup for call to sendto() which will send
		   * packet to remote over the UDP port.
		   */
		  int size;
		  ASSERT (addr_defined (&to_udp_addr));

		  /* In gremlin-test mode, we may choose to drop this packet */
		  if (!options->gremlin || ask_gremlin())
		    {
		      /*
		       * Let the traffic shaper know how many bytes
		       * we wrote.
		       */
#ifdef HAVE_GETTIMEOFDAY
		      if (options->shaper)
			shaper_wrote_bytes (&shaper, BLEN (&to_udp)
					    + datagram_overhead (ipv6_udp_transport));
#endif
#ifdef FRAGMENT_ENABLE
		      if (fragment)
			fragment_post_send (fragment, BLEN (&to_udp)
					    + datagram_overhead (ipv6_udp_transport));
#endif
		      /*
		       * Let the pinger know that we sent a packet.
		       */
		      if (options->ping_send_timeout)
			event_timeout_reset (&ping_send_interval, current);

#if PASSTOS_CAPABILITY
		      /* Set TOS */
		      if (ptos_defined)
			setsockopt(udp_socket.sd, IPPROTO_IP, IP_TOS, &ptos, sizeof(ptos));
#endif

		      /* Send packet */
		      size = sendto (udp_socket.sd, BPTR (&to_udp), BLEN (&to_udp), 0,
				     (struct sockaddr *) &to_udp_addr,
				     (socklen_t) sizeof (to_udp_addr));
		      if (size > 0)
			udp_write_bytes += size;
		    }
		  else
		    size = 0;

		  /* Check sendto() return status */
		  check_status (size, "write to UDP", &udp_socket);

		  if (size > 0)
		    {
		      /* Did we write a different size packet than we intended? */
		      if (size != BLEN (&to_udp))
			msg (D_LINK_ERRORS,
			     "UDP packet was truncated/expanded on write to %s (tried=%d,actual=%d)",
			     print_sockaddr (&to_udp_addr),
			     BLEN (&to_udp),
			     size);
		    }

		  /* Log packet send */
#ifdef LOG_RW
		  if (check_debug_level (D_LOG_RW) && !check_debug_level (D_LOG_RW + 1))
		    fprintf (stderr, "W");
#endif
		  msg (D_UDP_RW, "UDP WRITE [%d] to %s: %s",
		       BLEN (&to_udp), print_sockaddr (&to_udp_addr), PROTO_DUMP (&to_udp));
		}
	      else
		{
		  msg (D_LINK_ERRORS, "UDP packet too large on write to %s (tried=%d,max=%d)",
		       print_sockaddr (&to_udp_addr),
		       to_udp.len,
		       max_rw_size_udp);
		}

	      /*
	       * The free_to_udp flag means that we should free the packet buffer
	       * after send.  This flag is usually set when the TLS background
	       * thread generated the packet buffer.
	       */
	      if (free_to_udp)
		{
		  free_to_udp = false;
		  free_buf (&to_udp);
		}
	      to_udp = nullbuf;
	    }
	}
    }

  /*
   *  Do Cleanup
   */

  if (free_to_udp)
    free_buf (&to_udp);
    
#if defined(USE_CRYPTO) && defined(USE_SSL) && defined(USE_PTHREAD)
  if (tls_multi)
    tls_thread_close (&thread_parms);
#endif

  free_buf (&read_udp_buf);
  free_buf (&read_tun_buf);
  free_buf (&ping_buf);

#ifdef USE_LZO
  if (options->comp_lzo)
    {
      lzo_compress_uninit (&lzo_compwork);
      free_buf (&lzo_compress_buf);
      free_buf (&lzo_decompress_buf);
    }
#endif

#ifdef USE_CRYPTO

  free_buf (&encrypt_buf);
  free_buf (&decrypt_buf);

#ifdef USE_SSL
  if (tls_multi)
    tls_multi_free (tls_multi, true);

  if (data_channel_options)
    free (data_channel_options);

#endif
#endif /* USE_CRYPTO */

  /*
   * Free key schedules
   */
  if ( !(signal_received == SIGUSR1 && options->persist_key) )
    key_schedule_free (ks);

  /*
   * Close UDP connection
   */
  udp_socket_close (&udp_socket);
  if ( !(signal_received == SIGUSR1 && options->persist_remote_ip) )
    {
      CLEAR (udp_socket_addr->remote);
      CLEAR (udp_socket_addr->actual);
    }
  if ( !(signal_received == SIGUSR1 && options->persist_local_ip) )
    CLEAR (udp_socket_addr->local);

  /*
   * Close TUN/TAP device
   */
  if ( !(signal_received == SIGUSR1 && options->persist_tun) )
    {
      char* tuntap_actual = (char *) gc_malloc (sizeof (tuntap->actual));
      strcpy (tuntap_actual, tuntap->actual);

      msg (M_INFO, "Closing TUN/TAP device");
      close_tun (tuntap);

      /* Run the down script -- note that it will run at reduced
	 privilege if, for example, "--user nobody" was used. */
      run_script (options->down_script, tuntap_actual, TUN_MTU_SIZE (&frame),
		  max_rw_size_udp, options->ifconfig_local, options->ifconfig_remote);
    }

#ifdef USE_CRYPTO
  /*
   * Close packet-id persistance file
   */
  packet_id_persist_save (pid_persist);
  if ( !(signal_received == SIGUSR1) )
    packet_id_persist_close (pid_persist);
#endif

  /*
   * Close fragmentation handler.
   */
#ifdef FRAGMENT_ENABLE
  if (fragment)
    fragment_free (fragment);
#endif

 done:
  /* pop our garbage collection level */
  gc_free_level (gc_level);

  /* return the signal that brought us here */
  {
    int s = signal_received;
    signal_received = 0;
    return s;
  }
}

int
main (int argc, char *argv[])
{
  const int gc_level = gc_new_level ();
  bool first_time = true;
  int sig;

  init_random_seed();                  /* init random() function, only used as
					  source for weak random numbers */
  error_reset ();                      /* initialize error.c */
  reset_check_status ();               /* initialize status check code in socket.c */

#ifdef OPENVPN_DEBUG_COMMAND_LINE
  {
    int i;
    for (i = 0; i < argc; ++i)
      msg (M_INFO, "argv[%d] = '%s'", i, argv[i]);
  }
#endif

#ifdef PID_TEST
  packet_id_interactive_test();  /* test the sequence number code */
  goto exit;
#endif

  do {
    struct options options;
    init_options (&options);

    /*
     * Parse command line options,
     * and read configuration file.
     */
    parse_argv (&options, argc, argv);

    /*
     * OpenSSL info print mode?
     */
#ifdef USE_CRYPTO
    if (options.show_ciphers || options.show_digests
#ifdef USE_SSL
	|| options.show_tls_ciphers
#endif
	)
      {
	if (first_time)
	  init_ssl_lib ();
	if (options.show_ciphers)
	  show_available_ciphers ();
	if (options.show_digests)
	  show_available_digests ();
#ifdef USE_SSL
	if (options.show_tls_ciphers)
	  show_available_tls_ciphers ();
#endif
	free_ssl_lib ();
	goto exit;
      }

    /*
     * Possibly set --dev based on --dev-node.
     * For example, if --dev-node /tmp/foo/tun, and --dev undefined,
     * set --dev to tun.
     */
    if (!options.dev)
      options.dev = dev_component_in_dev_node (options.dev_node);

    /*
     * Static pre-shared key generation mode?
     */
    if (options.genkey)
      {
	struct key key;
	notnull (options.shared_secret_file,
		 "shared secret output file (--secret)");

	if (options.mlock)    /* should we disable paging? */
	  do_mlockall(true);

	generate_key_random (&key, NULL);
	write_key_file (&key, options.shared_secret_file);
	CLEAR (key);
	goto exit;
      }
#endif /* USE_CRYPTO */

    /*
     * Persistent TUN/TAP device management mode?
     */
#ifdef TUNSETPERSIST
    if (options.persist_config)
      {
	/* sanity check on options for --mktun or --rmtun */
	notnull (options.dev, "TUN/TAP device (--dev)");
	if (options.remote || options.ifconfig_local || options.ifconfig_remote
#ifdef USE_CRYPTO
	    || options.shared_secret_file
#ifdef USE_SSL
	    || options.tls_server || options.tls_client
#endif
#endif
	    )
	  msg (M_FATAL, "Options error: options --mktun or --rmtun should only be used together with --dev");
	tuncfg (options.dev, options.dev_type, options.dev_node,
		options.tun_ipv6, options.persist_mode);
	goto exit;
      }
#endif

    /*
     * Main OpenVPN block -- tunnel generation mode
     */
    {
#ifdef USE_CRYPTO
      if (options.test_crypto)
	{
	  notnull (options.shared_secret_file, "key file (--secret)");
	}
      else
#endif
	notnull (options.dev, "TUN/TAP device (--dev)");

      /*
       * Sanity check on daemon/inetd modes
       */

      if (options.daemon && options.inetd)
	{
	  msg (M_WARN, "Options error: only one of --daemon or --inetd may be specified");
	  usage_small ();
	}

      if (options.inetd && (options.local || options.remote))
	{
	  msg (M_WARN, "Options error: --local or --remote cannot be used with --inetd");
	  usage_small ();
	}

      /*
       * Sanity check on MTU parameters
       */
      if (options.tun_mtu_defined && options.udp_mtu_defined)
	{
	  msg (M_WARN, "Options error: only one of --tun-mtu or --udp-mtu may be defined (note that --ifconfig implies --udp-mtu %d)", UDP_MTU_DEFAULT);
	  usage_small ();
	}

      /*
       * If neither --tun-mtu or --udp-mtu specified,
       * use default --udp-mtu if --ifconfig specified, otherwise
       * use default --tun-mtu.
       */
      if (!options.tun_mtu_defined && !options.udp_mtu_defined)
	{
	  if (options.ifconfig_local || options.ifconfig_remote)
	    options.udp_mtu_defined = true;
	  else
	    options.tun_mtu_defined = true;
	}

      /*
       * Sanity check on --local, --remote, and ifconfig
       */
      if (string_defined_equal (options.local, options.remote)
	  && options.local_port == options.remote_port)
	{
	  msg (M_WARN, "Options error: --remote and --local addresses are the same");
	  usage_small ();
	}
	
      if (string_defined_equal (options.local, options.ifconfig_local)
	  || string_defined_equal (options.local, options.ifconfig_remote)
	  || string_defined_equal (options.remote, options.ifconfig_local)
	  || string_defined_equal (options.remote, options.ifconfig_remote))
	{
	  msg (M_WARN, "Options error: --local and --remote addresses must be distinct from --ifconfig addresses");
	  usage_small ();
	}

      if (string_defined_equal (options.ifconfig_local, options.ifconfig_remote))
	{
	  msg (M_WARN, "Options error: local and remote --ifconfig addresses must be different");
	  usage_small ();
	}

#ifdef USE_CRYPTO

      if (first_time)
	init_ssl_lib ();

#ifdef USE_SSL
      if (options.tls_server + options.tls_client +
	  (options.shared_secret_file != NULL) > 1)
	{
	  msg (M_WARN, "specify only one of --tls-server, --tls-client, or --secret");
	  usage_small ();
	}
      if (options.tls_server)
	{
	  notnull (options.dh_file, "DH file (--dh)");
	}
      if (options.tls_server || options.tls_client)
	{
	  notnull (options.ca_file, "CA file (--ca)");
	  notnull (options.cert_file, "certificate file (--cert)");
	  notnull (options.priv_key_file, "private key file (--key)");
	  if (first_time && options.askpass)
	    pem_password_callback (NULL, 0, 0, NULL);
	}
      else
	{
	  /*
	   * Make sure user doesn't specify any TLS options
	   * when in non-TLS mode.
	   */

#define MUST_BE_UNDEF(parm) if (options.parm != def.parm) msg (M_FATAL, err, #parm);

	  const char err[] = "Parameter %s can only be specified in TLS-mode, i.e. where --tls-server or --tls-client is also specified.";
	  struct options def;

	  init_options (&def);
	  MUST_BE_UNDEF (ca_file);
	  MUST_BE_UNDEF (dh_file);
	  MUST_BE_UNDEF (cert_file);
	  MUST_BE_UNDEF (priv_key_file);
	  MUST_BE_UNDEF (cipher_list);
	  MUST_BE_UNDEF (tls_verify);
	  MUST_BE_UNDEF (tls_timeout);
	  MUST_BE_UNDEF (renegotiate_bytes);
	  MUST_BE_UNDEF (renegotiate_packets);
	  MUST_BE_UNDEF (renegotiate_seconds);
	  MUST_BE_UNDEF (handshake_window);
	  MUST_BE_UNDEF (transition_window);
	  MUST_BE_UNDEF (tls_auth_file);
	  MUST_BE_UNDEF (single_session);
	  MUST_BE_UNDEF (disable_occ);
	}
#undef MUST_BE_UNDEF
#endif /* USE_CRYPTO */
#endif /* USE_SSL */

      set_check_status (D_LINK_ERRORS, D_READ_WRITE);
      set_debug_level (options.verbosity);
      set_mute_cutoff (options.mute);

      /* Become a daemon if requested */
      possibly_become_daemon (0, &options, first_time);

      /* show all option settings */
      show_settings (&options);

      /* Do Work */
      {
	/* these objects are potentially persistent across SIGUSR1 resets */
	struct udp_socket_addr usa;
	struct key_schedule ks;
	struct tuntap tuntap;
	struct packet_id_persist pid_persist;
	CLEAR (usa);
	CLEAR (ks);
	clear_tuntap (&tuntap);
	packet_id_persist_init (&pid_persist);

	do {
	  sig = openvpn (&options, &usa, &tuntap, &ks, &pid_persist, first_time);
	  first_time = false;
	} while (sig == SIGUSR1);
      }
    }
    gc_collect (gc_level);
    close_syslog ();
  } while (sig == SIGHUP);

  thread_cleanup();
#ifdef USE_CRYPTO
  free_ssl_lib ();
#endif

 exit:

#if defined(MEASURE_TLS_HANDSHAKE_STATS) && defined(USE_CRYPTO) && defined(USE_SSL)
  show_tls_performance_stats();
#endif

  /* pop our garbage collection level */
  gc_free_level (gc_level);

  return OPENVPN_EXIT_STATUS_GOOD; /* exit point */
}

/*
 * Basic threading test.
 */
#if defined(USE_PTHREAD) && defined(USE_CRYPTO)
static void*
test_crypto_thread (void *arg)
{
  struct udp_socket_addr usa;
  struct tuntap tuntap;
  struct key_schedule ks;
  struct packet_id_persist pid_persist;
  const struct options *opt = (struct options*) arg;

  set_nice (opt->nice_work);
  CLEAR (usa);
  CLEAR (ks);
  clear_tuntap (&tuntap);
  packet_id_persist_init (&pid_persist);
  openvpn (opt, &usa, &tuntap, &ks, &pid_persist, false);
  return NULL;
}
#endif
