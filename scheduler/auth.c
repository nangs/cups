/*
 * "$Id$"
 *
 *   Authorization routines for the Common UNIX Printing System (CUPS).
 *
 *   Copyright 1997-2005 by Easy Software Products, all rights reserved.
 *
 *   These coded instructions, statements, and computer programs are the
 *   property of Easy Software Products and are protected by Federal
 *   copyright law.  Distribution and use rights are outlined in the file
 *   "LICENSE.txt" which should have been included with this file.  If this
 *   file is missing or damaged please contact Easy Software Products
 *   at:
 *
 *       Attn: CUPS Licensing Information
 *       Easy Software Products
 *       44141 Airport View Drive, Suite 204
 *       Hollywood, Maryland 20636 USA
 *
 *       Voice: (301) 373-9600
 *       EMail: cups-info@cups.org
 *         WWW: http://www.cups.org
 *
 * Contents:
 *
 *   cupsdAddLocation()         - Add a location for authorization.
 *   cupsdAddName()             - Add a name to a location...
 *   cupsdAllowHost()           - Add a host name that is allowed to access the
 *                           location.
 *   cupsdAllowIP()             - Add an IP address or network that is allowed to
 *                           access the location.
 *   cupsdCheckAuth()           - Check authorization masks.
 *   cupsdCheckGroup()     - Check for a user's group membership.
 *   cupsdCopyLocation()        - Make a copy of a location...
 *   cupsdDeleteAllLocations()  - Free all memory used for location authorization.
 *   cupsdDeleteLocation() - Free all memory used by a location.
 *   cupsdDenyHost()            - Add a host name that is not allowed to access the
 *                           location.
 *   cupsdDenyIP()              - Add an IP address or network that is not allowed
 *                           to access the location.
 *   cupsdFindBest()            - Find the location entry that best matches the
 *                           resource.
 *   cupsdFindLocation()        - Find the named location.
 *   cupsdGetMD5Passwd()        - Get an MD5 password.
 *   cupsdIsAuthorized()   - Check to see if the user is authorized...
 *   add_allow()           - Add an allow mask to the location.
 *   add_deny()            - Add a deny mask to the location.
 *   cups_crypt()          - Encrypt the password using the DES or MD5
 *                           algorithms, as needed.
 *   pam_func()            - PAM conversation function.
 *   to64()                - Base64-encode an integer value...
 */

/*
 * Include necessary headers...
 */

#include "cupsd.h"
#include <grp.h>
#ifdef HAVE_SHADOW_H
#  include <shadow.h>
#endif /* HAVE_SHADOW_H */
#ifdef HAVE_CRYPT_H
#  include <crypt.h>
#endif /* HAVE_CRYPT_H */
#if HAVE_LIBPAM
#  ifdef HAVE_PAM_PAM_APPL_H
#    include <pam/pam_appl.h>
#  else
#    include <security/pam_appl.h>
#  endif /* HAVE_PAM_PAM_APPL_H */
#endif /* HAVE_LIBPAM */
#ifdef HAVE_USERSEC_H
#  include <usersec.h>
#endif /* HAVE_USERSEC_H */


/*
 * Local functions...
 */

static cupsd_authmask_t	*add_allow(cupsd_location_t *loc);
static cupsd_authmask_t	*add_deny(cupsd_location_t *loc);
#if !HAVE_LIBPAM
static char		*cups_crypt(const char *pw, const char *salt);
#endif /* !HAVE_LIBPAM */
#if HAVE_LIBPAM
static int		pam_func(int, const struct pam_message **,
			         struct pam_response **, void *);
#else
static void		to64(char *s, unsigned long v, int n);
#endif /* HAVE_LIBPAM */


/*
 * Local globals...
 */

#if defined(__hpux) && defined(HAVE_LIBPAM)
static cupsd_client_t		*auth_client;	/* Current client being authenticated */
#endif /* __hpux && HAVE_LIBPAM */


/*
 * 'cupsdAddLocation()' - Add a location for authorization.
 */

cupsd_location_t *				/* O - Pointer to new location record */
cupsdAddLocation(const char *location)	/* I - Location path */
{
  cupsd_location_t	*temp;			/* New location */


 /*
  * Try to allocate memory for the new location.
  */

  if (NumLocations == 0)
    temp = malloc(sizeof(cupsd_location_t));
  else
    temp = realloc(Locations, sizeof(cupsd_location_t) * (NumLocations + 1));

  if (temp == NULL)
    return (NULL);

  Locations = temp;
  temp      += NumLocations;
  NumLocations ++;

 /*
  * Initialize the record and copy the name over...
  */

  memset(temp, 0, sizeof(cupsd_location_t));
  strlcpy(temp->location, location, sizeof(temp->location));
  temp->length = strlen(temp->location);

  cupsdLogMessage(L_DEBUG, "cupsdAddLocation: added location \'%s\'", location);

 /*
  * Return the new record...
  */

  return (temp);
}


/*
 * 'cupsdAddName()' - Add a name to a location...
 */

void
cupsdAddName(cupsd_location_t *loc,	/* I - Location to add to */
        char       *name)	/* I - Name to add */
{
  char	**temp;			/* Pointer to names array */


  cupsdLogMessage(L_DEBUG2, "cupsdAddName(loc=%p, name=\"%s\")", loc, name);

  if (loc->num_names == 0)
    temp = malloc(sizeof(char *));
  else
    temp = realloc(loc->names, (loc->num_names + 1) * sizeof(char *));

  if (temp == NULL)
  {
    cupsdLogMessage(L_ERROR, "Unable to add name to location %s: %s", loc->location,
               strerror(errno));
    return;
  }

  loc->names = temp;

  if ((temp[loc->num_names] = strdup(name)) == NULL)
  {
    cupsdLogMessage(L_ERROR, "Unable to duplicate name for location %s: %s",
               loc->location, strerror(errno));
    return;
  }

  loc->num_names ++;
}


/*
 * 'cupsdAllowHost()' - Add a host name that is allowed to access the location.
 */

void
cupsdAllowHost(cupsd_location_t *loc,	/* I - Location to add to */
          char       *name)	/* I - Name of host or domain to add */
{
  cupsd_authmask_t	*temp;		/* New host/domain mask */
  char		ifname[32],	/* Interface name */
		*ifptr;		/* Pointer to end of name */


  cupsdLogMessage(L_DEBUG2, "cupsdAllowHost(loc=%p(%s), name=\"%s\")", loc,
             loc->location, name);

  if ((temp = add_allow(loc)) == NULL)
    return;

  if (strcasecmp(name, "@LOCAL") == 0)
  {
   /*
    * Allow *interface*...
    */

    temp->type             = AUTH_INTERFACE;
    temp->mask.name.name   = strdup("*");
    temp->mask.name.length = 1;
  }
  else if (strncasecmp(name, "@IF(", 4) == 0)
  {
   /*
    * Allow *interface*...
    */

    strlcpy(ifname, name + 4, sizeof(ifname));

    ifptr = ifname + strlen(ifname);

    if (ifptr[-1] == ')')
    {
      ifptr --;
      *ifptr = '\0';
    }

    temp->type             = AUTH_INTERFACE;
    temp->mask.name.name   = strdup(ifname);
    temp->mask.name.length = ifptr - ifname;
  }
  else
  {
   /*
    * Allow name...
    */

    temp->type             = AUTH_NAME;
    temp->mask.name.name   = strdup(name);
    temp->mask.name.length = strlen(name);
  }
}


/*
 * 'cupsdAllowIP()' - Add an IP address or network that is allowed to access the
 *               location.
 */

void
cupsdAllowIP(cupsd_location_t *loc,	/* I - Location to add to */
        unsigned   address[4],	/* I - IP address to add */
        unsigned   netmask[4])	/* I - Netmask of address */
{
  cupsd_authmask_t	*temp;		/* New host/domain mask */


  cupsdLogMessage(L_DEBUG2, "cupsdAllowIP(loc=%p(%s), address=%x:%x:%x:%x, netmask=%x:%x:%x:%x)",
	     loc, loc->location, address[0], address[1], address[2],
	     address[3], netmask[0], netmask[1], netmask[2],
	     netmask[3]);

  if ((temp = add_allow(loc)) == NULL)
    return;

  temp->type = AUTH_IP;
  memcpy(temp->mask.ip.address, address, sizeof(temp->mask.ip.address));
  memcpy(temp->mask.ip.netmask, netmask, sizeof(temp->mask.ip.netmask));
}


/*
 * 'cupsdCheckAuth()' - Check authorization masks.
 */

int				/* O - 1 if mask matches, 0 otherwise */
cupsdCheckAuth(unsigned   ip[4],	/* I - Client address */
          char       *name,	/* I - Client hostname */
          int        name_len,	/* I - Length of hostname */
          int        num_masks, /* I - Number of masks */
          cupsd_authmask_t *masks)	/* I - Masks */
{
  int		i;		/* Looping var */
  cupsd_netif_t	*iface;		/* Network interface */
  unsigned	netip4;		/* IPv4 network address */
#ifdef AF_INET6
  unsigned	netip6[4];	/* IPv6 network address */
#endif /* AF_INET6 */

  while (num_masks > 0)
  {
    switch (masks->type)
    {
      case AUTH_INTERFACE :
         /*
	  * Check for a match with a network interface...
	  */

          netip4 = htonl(ip[3]);

#ifdef AF_INET6
          netip6[0] = htonl(ip[0]);
          netip6[1] = htonl(ip[1]);
          netip6[2] = htonl(ip[2]);
          netip6[3] = htonl(ip[3]);
#endif /* AF_INET6 */

          if (!strcmp(masks->mask.name.name, "*"))
	  {
	   /*
	    * Check against all local interfaces...
	    */

            cupsdNetIFUpdate();

	    for (iface = NetIFList; iface != NULL; iface = iface->next)
	    {
	     /*
	      * Only check local interfaces...
	      */

	      if (!iface->is_local)
	        continue;

              if (iface->address.addr.sa_family == AF_INET)
	      {
	       /*
	        * Check IPv4 address...
		*/

        	if ((netip4 & iface->mask.ipv4.sin_addr.s_addr) ==
	            (iface->address.ipv4.sin_addr.s_addr &
		     iface->mask.ipv4.sin_addr.s_addr))
		  return (1);
              }
#ifdef AF_INET6
	      else
	      {
	       /*
	        * Check IPv6 address...
		*/

        	for (i = 0; i < 4; i ++)
		  if ((netip6[i] & iface->mask.ipv6.sin6_addr.s6_addr32[i]) !=
		      (iface->address.ipv6.sin6_addr.s6_addr32[i] &
		       iface->mask.ipv6.sin6_addr.s6_addr32[i]))
		    break;

		if (i == 4)
		  return (1);
              }
#endif /* AF_INET6 */
	    }
	  }
	  else
	  {
	   /*
	    * Check the named interface...
	    */

            if ((iface = NetIFFind(masks->mask.name.name)) != NULL)
	    {
              if (iface->address.addr.sa_family == AF_INET)
	      {
	       /*
		* Check IPv4 address...
		*/

        	if ((netip4 & iface->mask.ipv4.sin_addr.s_addr) ==
	            (iface->address.ipv4.sin_addr.s_addr &
		     iface->mask.ipv4.sin_addr.s_addr))
		  return (1);
              }
#ifdef AF_INET6
	      else
	      {
	       /*
		* Check IPv6 address...
		*/

        	for (i = 0; i < 4; i ++)
		  if ((netip6[i] & iface->mask.ipv6.sin6_addr.s6_addr32[i]) !=
		      (iface->address.ipv6.sin6_addr.s6_addr32[i] &
		       iface->mask.ipv6.sin6_addr.s6_addr32[i]))
		    break;

		if (i == 4)
		  return (1);
              }
#endif /* AF_INET6 */
	    }
	  }
	  break;

      case AUTH_NAME :
         /*
	  * Check for exact name match...
	  */

          if (strcasecmp(name, masks->mask.name.name) == 0)
	    return (1);

         /*
	  * Check for domain match...
	  */

	  if (name_len >= masks->mask.name.length &&
	      masks->mask.name.name[0] == '.' &&
	      strcasecmp(name + name_len - masks->mask.name.length,
	                 masks->mask.name.name) == 0)
	    return (1);
          break;

      case AUTH_IP :
         /*
	  * Check for IP/network address match...
	  */

          for (i = 0; i < 4; i ++)
	    if ((ip[i] & masks->mask.ip.netmask[i]) != masks->mask.ip.address[i])
	      break;

	  if (i == 4)
	    return (1);
          break;
    }

    masks ++;
    num_masks --;
  }

  return (0);
}


/*
 * 'cupsdCheckGroup()' - Check for a user's group membership.
 */

int					/* O - 1 if user is a member, 0 otherwise */
cupsdCheckGroup(
    const char    *username,		/* I - User name */
    struct passwd *user,		/* I - System user info */
    const char    *groupname)		/* I - Group name */
{
  int			i;		/* Looping var */
  struct group		*group;		/* System group info */
  char			junk[33];	/* MD5 password (not used) */


  cupsdLogMessage(L_DEBUG2, "cupsdCheckGroup(username=\"%s\", user=%p, groupname=\"%s\")\n",
             username, user, groupname);

 /*
  * Validate input...
  */

  if (!username || !groupname)
    return (0);

 /*
  * Check to see if the user is a member of the named group...
  */

  group = getgrnam(groupname);
  endgrent();

  if (group != NULL)
  {
   /*
    * Group exists, check it...
    */

    for (i = 0; group->gr_mem[i]; i ++)
      if (!strcasecmp(username, group->gr_mem[i]))
	return (1);
  }

 /*
  * Group doesn't exist or user not in group list, check the group ID
  * against the user's group ID...
  */

  if (user && group && group->gr_gid == user->pw_gid)
    return (1);

 /*
  * Username not found, group not found, or user is not part of the
  * system group...  Check for a user and group in the MD5 password
  * file...
  */

  if (cupsdGetMD5Passwd(username, groupname, junk) != NULL)
    return (1);

 /*
  * If we get this far, then the user isn't part of the named group...
  */

  return (0);
}


/*
 * 'cupsdCopyLocation()' - Make a copy of a location...
 */

cupsd_location_t *			/* O - New location */
cupsdCopyLocation(cupsd_location_t **loc)	/* IO - Original location */
{
  int		i;		/* Looping var */
  int		locindex;	/* Index into Locations array */
  cupsd_location_t	*temp;		/* New location */
  char		location[HTTP_MAX_URI];	/* Location of resource */


 /*
  * Add the new location, updating the original location
  * pointer as needed...
  */

  locindex = *loc - Locations;

 /*
  * Use a local copy of location because cupsdAddLocation may cause
  * this memory to be moved...
  */

  strlcpy(location, (*loc)->location, sizeof(location));

  if ((temp = cupsdAddLocation(location)) == NULL)
    return (NULL);

  *loc = Locations + locindex;

 /*
  * Copy the information from the original location to the new one.
  */

  temp->limit      = (*loc)->limit;
  temp->order_type = (*loc)->order_type;
  temp->type       = (*loc)->type;
  temp->level      = (*loc)->level;
  temp->satisfy    = (*loc)->satisfy;
  temp->encryption = (*loc)->encryption;

  if ((temp->num_names  = (*loc)->num_names) > 0)
  {
   /*
    * Copy the names array...
    */

    if ((temp->names = calloc(temp->num_names, sizeof(char *))) == NULL)
    {
      cupsdLogMessage(L_ERROR, "cupsdCopyLocation: Unable to allocate memory for %d names: %s",
                 temp->num_names, strerror(errno));
      NumLocations --;
      return (NULL);
    }

    for (i = 0; i < temp->num_names; i ++)
      if ((temp->names[i] = strdup((*loc)->names[i])) == NULL)
      {
	cupsdLogMessage(L_ERROR, "cupsdCopyLocation: Unable to copy name \"%s\": %s",
                   (*loc)->names[i], strerror(errno));

	NumLocations --;
	return (NULL);
      }
  }

  if ((temp->num_allow  = (*loc)->num_allow) > 0)
  {
   /*
    * Copy allow rules...
    */

    if ((temp->allow = calloc(temp->num_allow, sizeof(cupsd_authmask_t))) == NULL)
    {
      cupsdLogMessage(L_ERROR, "cupsdCopyLocation: Unable to allocate memory for %d allow rules: %s",
                 temp->num_allow, strerror(errno));
      NumLocations --;
      return (NULL);
    }

    for (i = 0; i < temp->num_allow; i ++)
      switch (temp->allow[i].type = (*loc)->allow[i].type)
      {
        case AUTH_NAME :
	    temp->allow[i].mask.name.length = (*loc)->allow[i].mask.name.length;
	    temp->allow[i].mask.name.name   = strdup((*loc)->allow[i].mask.name.name);

            if (temp->allow[i].mask.name.name == NULL)
	    {
	      cupsdLogMessage(L_ERROR, "cupsdCopyLocation: Unable to copy allow name \"%s\": %s",
                	 (*loc)->allow[i].mask.name.name, strerror(errno));
	      NumLocations --;
	      return (NULL);
	    }
	    break;
	case AUTH_IP :
	    memcpy(&(temp->allow[i].mask.ip), &((*loc)->allow[i].mask.ip),
	           sizeof(cupsd_ipmask_t));
	    break;
      }
  }

  if ((temp->num_deny  = (*loc)->num_deny) > 0)
  {
   /*
    * Copy deny rules...
    */

    if ((temp->deny = calloc(temp->num_deny, sizeof(cupsd_authmask_t))) == NULL)
    {
      cupsdLogMessage(L_ERROR, "cupsdCopyLocation: Unable to allocate memory for %d deny rules: %s",
                 temp->num_deny, strerror(errno));
      NumLocations --;
      return (NULL);
    }

    for (i = 0; i < temp->num_deny; i ++)
      switch (temp->deny[i].type = (*loc)->deny[i].type)
      {
        case AUTH_NAME :
	    temp->deny[i].mask.name.length = (*loc)->deny[i].mask.name.length;
	    temp->deny[i].mask.name.name   = strdup((*loc)->deny[i].mask.name.name);

            if (temp->deny[i].mask.name.name == NULL)
	    {
	      cupsdLogMessage(L_ERROR, "cupsdCopyLocation: Unable to copy deny name \"%s\": %s",
                	 (*loc)->deny[i].mask.name.name, strerror(errno));
	      NumLocations --;
	      return (NULL);
	    }
	    break;
	case AUTH_IP :
	    memcpy(&(temp->deny[i].mask.ip), &((*loc)->deny[i].mask.ip),
	           sizeof(cupsd_ipmask_t));
	    break;
      }
  }

  return (temp);
}


/*
 * 'cupsdDeleteAllLocations()' - Free all memory used for location authorization.
 */

void
cupsdDeleteAllLocations(void)
{
  int		i;			/* Looping var */
  cupsd_location_t	*loc;			/* Current location */


 /*
  * Free all of the allow/deny records first...
  */

  for (i = NumLocations, loc = Locations; i > 0; i --, loc ++)
    cupsdDeleteLocation(loc);

 /*
  * Then free the location array...
  */

  if (NumLocations > 0)
    free(Locations);

  Locations    = NULL;
  NumLocations = 0;
}


/*
 * 'cupsdDeleteLocation()' - Free all memory used by a location.
 */

void
cupsdDeleteLocation(cupsd_location_t *loc)	/* I - Location to delete */
{
  int		i;			/* Looping var */
  cupsd_authmask_t	*mask;			/* Current mask */


  for (i = loc->num_names - 1; i >= 0; i --)
    free(loc->names[i]);

  if (loc->num_names > 0)
    free(loc->names);

  for (i = loc->num_allow, mask = loc->allow; i > 0; i --, mask ++)
    if (mask->type == AUTH_NAME || mask->type == AUTH_INTERFACE)
      free(mask->mask.name.name);

  if (loc->num_allow > 0)
    free(loc->allow);

  for (i = loc->num_deny, mask = loc->deny; i > 0; i --, mask ++)
    if (mask->type == AUTH_NAME || mask->type == AUTH_INTERFACE)
      free(mask->mask.name.name);

  if (loc->num_deny > 0)
    free(loc->deny);
}


/*
 * 'cupsdDenyHost()' - Add a host name that is not allowed to access the location.
 */

void
cupsdDenyHost(cupsd_location_t *loc,	/* I - Location to add to */
         char       *name)	/* I - Name of host or domain to add */
{
  cupsd_authmask_t	*temp;		/* New host/domain mask */
  char		ifname[32],	/* Interface name */
		*ifptr;		/* Pointer to end of name */


  cupsdLogMessage(L_DEBUG2, "cupsdDenyHost(loc=%p(%s), name=\"%s\")", loc,
             loc->location, name);

  if ((temp = add_deny(loc)) == NULL)
    return;

  if (strcasecmp(name, "@LOCAL") == 0)
  {
   /*
    * Deny *interface*...
    */

    temp->type             = AUTH_INTERFACE;
    temp->mask.name.name   = strdup("*");
    temp->mask.name.length = 1;
  }
  else if (strncasecmp(name, "@IF(", 4) == 0)
  {
   /*
    * Deny *interface*...
    */

    strlcpy(ifname, name + 4, sizeof(ifname));

    ifptr = ifname + strlen(ifname);

    if (ifptr[-1] == ')')
    {
      ifptr --;
      *ifptr = '\0';
    }

    temp->type             = AUTH_INTERFACE;
    temp->mask.name.name   = strdup(ifname);
    temp->mask.name.length = ifptr - ifname;
  }
  else
  {
   /*
    * Deny name...
    */

    temp->type             = AUTH_NAME;
    temp->mask.name.name   = strdup(name);
    temp->mask.name.length = strlen(name);
  }
}


/*
 * 'cupsdDenyIP()' - Add an IP address or network that is not allowed to access
 *              the location.
 */

void
cupsdDenyIP(cupsd_location_t *loc,		/* I - Location to add to */
       unsigned   address[4],	/* I - IP address to add */
       unsigned   netmask[4])	/* I - Netmask of address */
{
  cupsd_authmask_t	*temp;		/* New host/domain mask */


  cupsdLogMessage(L_DEBUG, "cupsdDenyIP(loc=%p(%s), address=%x:%x:%x:%x, netmask=%x:%x:%x:%x)",
	     loc, loc->location, address[0], address[1], address[2],
	     address[3], netmask[0], netmask[1], netmask[2],
	     netmask[3]);

  if ((temp = add_deny(loc)) == NULL)
    return;

  temp->type = AUTH_IP;
  memcpy(temp->mask.ip.address, address, sizeof(temp->mask.ip.address));
  memcpy(temp->mask.ip.netmask, netmask, sizeof(temp->mask.ip.netmask));
}


/*
 * 'cupsdFindBest()' - Find the location entry that best matches the resource.
 */

cupsd_location_t *				/* O - Location that matches */
cupsdFindBest(const char   *path,		/* I - Resource path */
         http_state_t state)		/* I - HTTP state/request */
{
  int		i;			/* Looping var */
  char		uri[HTTP_MAX_URI],	/* URI in request... */
		*uriptr;		/* Pointer into URI */
  cupsd_location_t	*loc,			/* Current location */
		*best;			/* Best match for location so far */
  int		bestlen;		/* Length of best match */
  int		limit;			/* Limit field */
  static const int limits[] =		/* Map http_status_t to AUTH_LIMIT_xyz */
		{
		  AUTH_LIMIT_ALL,
		  AUTH_LIMIT_OPTIONS,
		  AUTH_LIMIT_GET,
		  AUTH_LIMIT_GET,
		  AUTH_LIMIT_HEAD,
		  AUTH_LIMIT_POST,
		  AUTH_LIMIT_POST,
		  AUTH_LIMIT_POST,
		  AUTH_LIMIT_PUT,
		  AUTH_LIMIT_PUT,
		  AUTH_LIMIT_DELETE,
		  AUTH_LIMIT_TRACE,
		  AUTH_LIMIT_ALL,
		  AUTH_LIMIT_ALL
		};


 /*
  * First copy the connection URI to a local string so we have drop
  * any .ppd extension from the pathname in /printers or /classes
  * URIs...
  */

  strlcpy(uri, path, sizeof(uri));

  if (!strncmp(uri, "/printers/", 10) ||
      !strncmp(uri, "/classes/", 9))
  {
   /*
    * Check if the URI has .ppd on the end...
    */

    uriptr = uri + strlen(uri) - 4; /* len > 4 if we get here... */

    if (!strcmp(uriptr, ".ppd"))
      *uriptr = '\0';
  }

  cupsdLogMessage(L_DEBUG2, "cupsdFindBest: uri = \"%s\"...", uri);

 /*
  * Loop through the list of locations to find a match...
  */

  limit   = limits[state];
  best    = NULL;
  bestlen = 0;

  for (i = NumLocations, loc = Locations; i > 0; i --, loc ++)
  {
    cupsdLogMessage(L_DEBUG2, "cupsdFindBest: Location %s Limit %x",
               loc->location, loc->limit);

    if (!strncmp(uri, "/printers/", 10) || !strncmp(uri, "/classes/", 9))
    {
     /*
      * Use case-insensitive comparison for queue names...
      */

      if (loc->length > bestlen &&
          strncasecmp(uri, loc->location, loc->length) == 0 &&
	  loc->location[0] == '/' &&
	  (limit & loc->limit) != 0)
      {
	best    = loc;
	bestlen = loc->length;
      }
    }
    else
    {
     /*
      * Use case-sensitive comparison for other URIs...
      */

      if (loc->length > bestlen &&
          !strncmp(uri, loc->location, loc->length) &&
	  loc->location[0] == '/' &&
	  (limit & loc->limit) != 0)
      {
	best    = loc;
	bestlen = loc->length;
      }
    }
  }

 /*
  * Return the match, if any...
  */

  cupsdLogMessage(L_DEBUG2, "cupsdFindBest: best = %s", best ? best->location : "NONE");

  return (best);
}


/*
 * 'cupsdFindLocation()' - Find the named location.
 */

cupsd_location_t *				/* O - Location that matches */
cupsdFindLocation(const char *location)	/* I - Connection */
{
  int		i;			/* Looping var */


 /*
  * Loop through the list of locations to find a match...
  */

  for (i = 0; i < NumLocations; i ++)
    if (strcasecmp(Locations[i].location, location) == 0)
      return (Locations + i);

  return (NULL);
}


/*
 * 'cupsdGetMD5Passwd()' - Get an MD5 password.
 */

char *					/* O - MD5 password string */
cupsdGetMD5Passwd(const char *username,	/* I - Username */
             const char *group,		/* I - Group */
             char       passwd[33])	/* O - MD5 password string */
{
  cups_file_t	*fp;			/* passwd.md5 file */
  char		filename[1024],		/* passwd.md5 filename */
		line[256],		/* Line from file */
		tempuser[33],		/* User from file */
		tempgroup[33];		/* Group from file */


  cupsdLogMessage(L_DEBUG2, "cupsdGetMD5Passwd(username=\"%s\", group=\"%s\", passwd=%p)",
             username, group ? group : "(null)", passwd);

  snprintf(filename, sizeof(filename), "%s/passwd.md5", ServerRoot);
  if ((fp = cupsFileOpen(filename, "r")) == NULL)
  {
    cupsdLogMessage(L_ERROR, "Unable to open %s - %s", filename, strerror(errno));
    return (NULL);
  }

  while (cupsFileGets(fp, line, sizeof(line)) != NULL)
  {
    if (sscanf(line, "%32[^:]:%32[^:]:%32s", tempuser, tempgroup, passwd) != 3)
    {
      cupsdLogMessage(L_ERROR, "Bad MD5 password line: %s", line);
      continue;
    }

    if (strcmp(username, tempuser) == 0 &&
        (group == NULL || strcmp(group, tempgroup) == 0))
    {
     /*
      * Found the password entry!
      */

      cupsdLogMessage(L_DEBUG2, "Found MD5 user %s, group %s...", username,
                 tempgroup);

      cupsFileClose(fp);
      return (passwd);
    }
  }

 /*
  * Didn't find a password entry - return NULL!
  */

  cupsFileClose(fp);
  return (NULL);
}


/*
 * 'cupsdIsAuthorized()' - Check to see if the user is authorized...
 */

http_status_t				/* O - HTTP_OK if authorized or error code */
cupsdIsAuthorized(cupsd_client_t   *con,	/* I - Connection */
                  const char *owner)	/* I - Owner of object */
{
  int		i, j,			/* Looping vars */
		auth;			/* Authorization status */
  unsigned	address[4];		/* Authorization address */
  cupsd_location_t	*best;			/* Best match for location so far */
  int		hostlen;		/* Length of hostname */
  struct passwd	*pw;			/* User password data */
  char		nonce[HTTP_MAX_VALUE],	/* Nonce value from client */
		md5[33],		/* MD5 password */
		basicmd5[33];		/* MD5 of Basic password */
#if HAVE_LIBPAM
  pam_handle_t	*pamh;			/* PAM authentication handle */
  int		pamerr;			/* PAM error code */
  struct pam_conv pamdata;		/* PAM conversation data */
#elif defined(HAVE_USERSEC_H)
  char		*authmsg;		/* Authentication message */
  char		*loginmsg;		/* Login message */
  int		reenter;		/* ??? */
#else
  char		*pass;			/* Encrypted password */
#  ifdef HAVE_SHADOW_H
  struct spwd	*spw;			/* Shadow password data */
#  endif /* HAVE_SHADOW_H */
#endif /* HAVE_LIBPAM */
  static const char * const states[] =	/* HTTP client states... */
		{
		  "WAITING",
		  "OPTIONS",
		  "GET",
		  "GET",
		  "HEAD",
		  "POST",
		  "POST",
		  "POST",
		  "PUT",
		  "PUT",
		  "DELETE",
		  "TRACE",
		  "CLOSE",
		  "STATUS"
		};
  static const char * const levels[] =	/* Auth levels */
		{
		  "ANON",
		  "USER",
		  "GROUP"
		};
  static const char * const types[] =	/* Auth types */
		{
		  "NONE",
		  "BASIC",
		  "DIGEST",
		  "BASICDIGEST"
		};


  cupsdLogMessage(L_DEBUG2, "cupsdIsAuthorized: con->uri=\"%s\", con->best=%p(%s)",
             con->uri, con->best, con->best ? con->best->location : "");

 /*
  * If there is no "best" authentication rule for this request, then
  * access is allowed from the local system and denied from other
  * addresses...
  */

  if (!con->best)
  {
    if (!strcmp(con->http.hostname, "localhost") ||
        !strcmp(con->http.hostname, ServerName))
      return (HTTP_OK);
    else
      return (HTTP_FORBIDDEN);
  }

  best = con->best;

  cupsdLogMessage(L_DEBUG2, "cupsdIsAuthorized: level=AUTH_%s, type=AUTH_%s, satisfy=AUTH_SATISFY_%s, num_names=%d",
             levels[best->level], types[best->type],
	     best->satisfy ? "ANY" : "ALL", best->num_names);

  if (best->limit == AUTH_LIMIT_IPP)
    cupsdLogMessage(L_DEBUG2, "cupsdIsAuthorized: op=%x(%s)", best->op,
               ippOpString(best->op));

 /*
  * Check host/ip-based accesses...
  */

#ifdef AF_INET6
  if (con->http.hostaddr.addr.sa_family == AF_INET6)
  {
   /*
    * Copy IPv6 address...
    */

    address[0] = ntohl(con->http.hostaddr.ipv6.sin6_addr.s6_addr32[0]);
    address[1] = ntohl(con->http.hostaddr.ipv6.sin6_addr.s6_addr32[1]);
    address[2] = ntohl(con->http.hostaddr.ipv6.sin6_addr.s6_addr32[2]);
    address[3] = ntohl(con->http.hostaddr.ipv6.sin6_addr.s6_addr32[3]);
  }
  else
#endif /* AF_INET6 */
  if (con->http.hostaddr.addr.sa_family == AF_INET)
  {
   /*
    * Copy IPv4 address...
    */

    address[0] = 0;
    address[1] = 0;
    address[2] = 0;
    address[3] = ntohl(con->http.hostaddr.ipv4.sin_addr.s_addr);
  }
  else
    memset(address, 0, sizeof(address));

  hostlen = strlen(con->http.hostname);

  if (!strcasecmp(con->http.hostname, "localhost"))
  {
   /*
    * Access from localhost (127.0.0.1 or :::1) is always allowed...
    */

    auth = AUTH_ALLOW;
  }
  else
  {
   /*
    * Do authorization checks on the domain/address...
    */

    switch (best->order_type)
    {
      default :
	  auth = AUTH_DENY;	/* anti-compiler-warning-code */
	  break;

      case AUTH_ALLOW : /* Order Deny,Allow */
          auth = AUTH_ALLOW;

          if (cupsdCheckAuth(address, con->http.hostname, hostlen,
	          	best->num_deny, best->deny))
	    auth = AUTH_DENY;

          if (cupsdCheckAuth(address, con->http.hostname, hostlen,
	        	best->num_allow, best->allow))
	    auth = AUTH_ALLOW;
	  break;

      case AUTH_DENY : /* Order Allow,Deny */
          auth = AUTH_DENY;

          if (cupsdCheckAuth(address, con->http.hostname, hostlen,
	        	best->num_allow, best->allow))
	    auth = AUTH_ALLOW;

          if (cupsdCheckAuth(address, con->http.hostname, hostlen,
	        	best->num_deny, best->deny))
	    auth = AUTH_DENY;
	  break;
    }
  }

  cupsdLogMessage(L_DEBUG2, "cupsdIsAuthorized: auth=AUTH_%s...",
             auth ? "DENY" : "ALLOW");

  if (auth == AUTH_DENY && best->satisfy == AUTH_SATISFY_ALL)
    return (HTTP_FORBIDDEN);

#ifdef HAVE_SSL
 /*
  * See if encryption is required...
  */

  if (best->encryption >= HTTP_ENCRYPT_REQUIRED && !con->http.tls)
  {
    cupsdLogMessage(L_DEBUG2, "cupsdIsAuthorized: Need upgrade to TLS...");
    return (HTTP_UPGRADE_REQUIRED);
  }
#endif /* HAVE_SSL */

 /*
  * Now see what access level is required...
  */

  if (best->level == AUTH_ANON ||	/* Anonymous access - allow it */
      (best->type == AUTH_NONE && best->num_names == 0))
    return (HTTP_OK);

  if (best->type == AUTH_NONE && best->limit == AUTH_LIMIT_IPP)
  {
   /*
    * Check for unauthenticated username...
    */

    ipp_attribute_t	*attr;		/* requesting-user-name attribute */


    attr = ippFindAttribute(con->request, "requesting-user-name", IPP_TAG_NAME);
    if (attr)
    {
      cupsdLogMessage(L_DEBUG2, "cupsdIsAuthorized: requesting-user-name=\"%s\"",
                 attr->values[0].string.text);
      return (HTTP_OK);
    }
  }

  cupsdLogMessage(L_DEBUG2, "cupsdIsAuthorized: username=\"%s\" password=%d chars",
	     con->username, (int)strlen(con->password));
  DEBUG_printf(("cupsdIsAuthorized: username=\"%s\", password=\"%s\"\n",
		con->username, con->password));

  if (!con->username[0])
  {
    if (best->satisfy == AUTH_SATISFY_ALL || auth == AUTH_DENY)
      return (HTTP_UNAUTHORIZED);	/* Non-anonymous needs user/pass */
    else
      return (HTTP_OK);			/* unless overridden with Satisfy */
  }

 /*
  * Check the user's password...
  */

  cupsdLogMessage(L_DEBUG2, "cupsdIsAuthorized: Checking \"%s\", address = %x:%x:%x:%x, hostname = \"%s\"",
	     con->username, address[0], address[1], address[2],
	     address[3], con->http.hostname);

  pw = NULL;

  if (strcasecmp(con->http.hostname, "localhost") ||
      strncmp(con->http.fields[HTTP_FIELD_AUTHORIZATION], "Local", 5))
  {
   /*
    * Not doing local certificate-based authentication; check the password...
    */

    if (!con->password[0])
      return (HTTP_UNAUTHORIZED);

   /*
    * See what kind of authentication we are doing...
    */

    switch (best->type != AUTH_NONE ? best->type : DefaultAuthType)
    {
      case AUTH_BASIC :
	 /*
	  * Get the user info...
	  */

	  pw = getpwnam(con->username);		/* Get the current password */
	  endpwent();				/* Close the password file */

#if HAVE_LIBPAM
	 /*
	  * Only use PAM to do authentication.  This allows MD5 passwords, among
	  * other things...
	  */

	  pamdata.conv        = pam_func;
	  pamdata.appdata_ptr = con;

#  ifdef __hpux
	 /*
	  * Workaround for HP-UX bug in pam_unix; see pam_conv() below for
	  * more info...
	  */

	  auth_client = con;
#  endif /* __hpux */

	  DEBUG_printf(("cupsdIsAuthorized: Setting appdata_ptr = %p\n", con));

	  pamerr = pam_start("cups", con->username, &pamdata, &pamh);
	  if (pamerr != PAM_SUCCESS)
	  {
	    cupsdLogMessage(L_ERROR, "cupsdIsAuthorized: pam_start() returned %d (%s)!\n",
        	       pamerr, pam_strerror(pamh, pamerr));
	    pam_end(pamh, 0);
	    return (HTTP_UNAUTHORIZED);
	  }

	  pamerr = pam_authenticate(pamh, PAM_SILENT);
	  if (pamerr != PAM_SUCCESS)
	  {
	    cupsdLogMessage(L_ERROR, "cupsdIsAuthorized: pam_authenticate() returned %d (%s)!\n",
        	       pamerr, pam_strerror(pamh, pamerr));
	    pam_end(pamh, 0);
	    return (HTTP_UNAUTHORIZED);
	  }

	  pamerr = pam_acct_mgmt(pamh, PAM_SILENT);
	  if (pamerr != PAM_SUCCESS)
	  {
	    cupsdLogMessage(L_ERROR, "cupsdIsAuthorized: pam_acct_mgmt() returned %d (%s)!\n",
        	       pamerr, pam_strerror(pamh, pamerr));
	    pam_end(pamh, 0);
	    return (HTTP_UNAUTHORIZED);
	  }

	  pam_end(pamh, PAM_SUCCESS);
#elif defined(HAVE_USERSEC_H)
	 /*
	  * Use AIX authentication interface...
	  */

	  cupsdLogMessage(L_DEBUG, "cupsdIsAuthorized: AIX authenticate of username \"%s\"",
                     con->username);

	  reenter = 1;
	  if (authenticate(con->username, con->password, &reenter, &authmsg) != 0)
	  {
	    cupsdLogMessage(L_DEBUG, "cupsdIsAuthorized: Unable to authenticate username \"%s\": %s",
	               con->username, strerror(errno));
	    return (HTTP_UNAUTHORIZED);
	  }
#else
         /*
	  * Use normal UNIX password file-based authentication...
	  */

	  if (pw == NULL)			/* No such user... */
	  {
	    cupsdLogMessage(L_WARN, "cupsdIsAuthorized: Unknown username \"%s\"; access denied.",
        	       con->username);
	    return (HTTP_UNAUTHORIZED);
	  }

#  ifdef HAVE_SHADOW_H
	  spw = getspnam(con->username);
	  endspent();

	  if (spw == NULL && strcmp(pw->pw_passwd, "x") == 0)
	  {					/* Don't allow blank passwords! */
	    cupsdLogMessage(L_WARN, "cupsdIsAuthorized: Username \"%s\" has no shadow password; access denied.",
        	       con->username);
	    return (HTTP_UNAUTHORIZED);	/* No such user or bad shadow file */
	  }

#    ifdef DEBUG
	  if (spw != NULL)
	    printf("spw->sp_pwdp = \"%s\"\n", spw->sp_pwdp);
	  else
	    puts("spw = NULL");
#    endif /* DEBUG */

	  if (spw != NULL && spw->sp_pwdp[0] == '\0' && pw->pw_passwd[0] == '\0')
#  else
	  if (pw->pw_passwd[0] == '\0')
#  endif /* HAVE_SHADOW_H */
	  {					/* Don't allow blank passwords! */
	    cupsdLogMessage(L_WARN, "cupsdIsAuthorized: Username \"%s\" has no password; access denied.",
        	       con->username);
	    return (HTTP_UNAUTHORIZED);
	  }

	 /*
	  * OK, the password isn't blank, so compare with what came from the client...
	  */

	  pass = cups_crypt(con->password, pw->pw_passwd);

	  cupsdLogMessage(L_DEBUG2, "cupsdIsAuthorized: pw_passwd = %s, crypt = %s",
		     pw->pw_passwd, pass);

	  if (pass == NULL || strcmp(pw->pw_passwd, pass) != 0)
	  {
#  ifdef HAVE_SHADOW_H
	    if (spw != NULL)
	    {
	      pass = cups_crypt(con->password, spw->sp_pwdp);

	      cupsdLogMessage(L_DEBUG2, "cupsdIsAuthorized: sp_pwdp = %s, crypt = %s",
			 spw->sp_pwdp, pass);

	      if (pass == NULL || strcmp(spw->sp_pwdp, pass) != 0)
		return (HTTP_UNAUTHORIZED);
	    }
	    else
#  endif /* HAVE_SHADOW_H */
	      return (HTTP_UNAUTHORIZED);
	  }
#endif /* HAVE_LIBPAM */
          break;

      case AUTH_DIGEST :
	 /*
	  * Do Digest authentication...
	  */

	  if (!httpGetSubField(&(con->http), HTTP_FIELD_AUTHORIZATION, "nonce",
                               nonce))
	  {
            cupsdLogMessage(L_ERROR, "cupsdIsAuthorized: No nonce value for Digest authentication!");
            return (HTTP_UNAUTHORIZED);
	  }

	  if (strcmp(con->http.hostname, nonce) != 0)
	  {
            cupsdLogMessage(L_ERROR, "cupsdIsAuthorized: Nonce value error!");
            cupsdLogMessage(L_ERROR, "cupsdIsAuthorized: Expected \"%s\",",
	               con->http.hostname);
            cupsdLogMessage(L_ERROR, "cupsdIsAuthorized: Got \"%s\"!", nonce);
            return (HTTP_UNAUTHORIZED);
	  }

	  cupsdLogMessage(L_DEBUG2, "cupsdIsAuthorized: nonce = \"%s\"", nonce);

	  if (best->num_names && best->level == AUTH_GROUP)
	  {
	    cupsdLogMessage(L_DEBUG2, "cupsdIsAuthorized: num_names = %d", best->num_names);

            for (i = 0; i < best->num_names; i ++)
	    {
	      if (!strcasecmp(best->names[i], "@SYSTEM"))
	      {
	        for (j = 0; j < NumSystemGroups; j ++)
		  if (cupsdGetMD5Passwd(con->username, SystemGroups[j], md5))
		    break;

                if (j < NumSystemGroups)
		  break;
	      }
	      else if (cupsdGetMD5Passwd(con->username, best->names[i], md5))
		break;
            }

            if (i >= best->num_names)
	      md5[0] = '\0';
	  }
	  else if (!cupsdGetMD5Passwd(con->username, NULL, md5))
	    md5[0] = '\0';

	  if (!md5[0])
	  {
            cupsdLogMessage(L_DEBUG2, "cupsdIsAuthorized: No matching user:group for \"%s\" in passwd.md5!",
	               con->username);
            return (HTTP_UNAUTHORIZED);
	  }

	  httpMD5Final(nonce, states[con->http.state], con->uri, md5);

	  if (strcmp(md5, con->password) != 0)
	  {
            cupsdLogMessage(L_DEBUG2, "cupsdIsAuthorized: MD5s \"%s\" and \"%s\" don't match!",
	               md5, con->password);
            return (HTTP_UNAUTHORIZED);
	  }
          break;

      case AUTH_BASICDIGEST :
         /*
	  * Do Basic authentication with the Digest password file...
	  */

	  if (best->num_names && best->level == AUTH_GROUP)
	  {
	    cupsdLogMessage(L_DEBUG2, "cupsdIsAuthorized: num_names = %d", best->num_names);

            for (i = 0; i < best->num_names; i ++)
	    {
	      if (!strcasecmp(best->names[i], "@SYSTEM"))
	      {
	        for (j = 0; j < NumSystemGroups; j ++)
		  if (cupsdGetMD5Passwd(con->username, SystemGroups[j], md5))
		    break;

                if (j < NumSystemGroups)
		  break;
	      }
	      else if (cupsdGetMD5Passwd(con->username, best->names[i], md5))
		break;
            }

            if (i >= best->num_names)
	      md5[0] = '\0';
	  }
	  else if (!cupsdGetMD5Passwd(con->username, NULL, md5))
	    md5[0] = '\0';

	  if (!md5[0])
	  {
            cupsdLogMessage(L_DEBUG2, "cupsdIsAuthorized: No matching user:group for \"%s\" in passwd.md5!",
	               con->username);
            return (HTTP_UNAUTHORIZED);
	  }

	  httpMD5(con->username, "CUPS", con->password, basicmd5);

	  if (strcmp(md5, basicmd5) != 0)
	  {
            cupsdLogMessage(L_DEBUG2, "cupsdIsAuthorized: MD5s \"%s\" and \"%s\" don't match!",
	               md5, basicmd5);
            return (HTTP_UNAUTHORIZED);
	  }
	  break;
    }
  }
  else
  {
   /*
    * Get password entry for certificate-based auth...
    */

    pw = getpwnam(con->username);	/* Get the current password */
    endpwent();				/* Close the password file */
  }

 /*
  * OK, the password is good.  See if we need normal user access, or group
  * access... (root always matches)
  */

  if (!strcmp(con->username, "root"))
    return (HTTP_OK);

  if (best->level == AUTH_USER)
  {
   /*
    * If there are no names associated with this location, then
    * any valid user is OK...
    */

    cupsdLogMessage(L_DEBUG2, "cupsdIsAuthorized: Checking user membership...");

    if (best->num_names == 0)
      return (HTTP_OK);

   /*
    * Otherwise check the user list and return OK if this user is
    * allowed...
    */

    for (i = 0; i < best->num_names; i ++)
    {
      if (!strcasecmp(best->names[i], "@OWNER") && owner &&
          !strcasecmp(con->username, owner))
	return (HTTP_OK);
      else if (!strcasecmp(best->names[i], "@SYSTEM"))
      {
        for (j = 0; j < NumSystemGroups; j ++)
	  if (cupsdCheckGroup(con->username, pw, SystemGroups[j]))
	    return (HTTP_OK);
      }
      else if (best->names[i][0] == '@')
      {
        if (cupsdCheckGroup(con->username, pw, best->names[i] + 1))
          return (HTTP_OK);
      }
      else if (!strcasecmp(con->username, best->names[i]))
        return (HTTP_OK);
    }

    return (HTTP_UNAUTHORIZED);
  }

 /*
  * Check to see if this user is in any of the named groups...
  */

  cupsdLogMessage(L_DEBUG2, "cupsdIsAuthorized: Checking group membership...");

  if (best->type == AUTH_BASIC)
  {
   /*
    * Check to see if this user is in any of the named groups...
    */

    for (i = 0; i < best->num_names; i ++)
    {
      cupsdLogMessage(L_DEBUG2, "cupsdIsAuthorized: Checking group \"%s\" membership...",
                 best->names[i]);

      if (!strcasecmp(best->names[i], "@SYSTEM"))
      {
        for (j = 0; j < NumSystemGroups; j ++)
	  if (cupsdCheckGroup(con->username, pw, SystemGroups[j]))
	    return (HTTP_OK);
      }
      else if (cupsdCheckGroup(con->username, pw, best->names[i]))
        return (HTTP_OK);
    }

   /*
    * The user isn't part of the specified group, so deny access...
    */

    cupsdLogMessage(L_DEBUG2, "cupsdIsAuthorized: User not in group(s)!");

    return (HTTP_UNAUTHORIZED);
  }

 /*
  * All checks passed...
  */

  return (HTTP_OK);
}


/*
 * 'add_allow()' - Add an allow mask to the location.
 */

static cupsd_authmask_t *		/* O - New mask record */
add_allow(cupsd_location_t *loc)	/* I - Location to add to */
{
  cupsd_authmask_t	*temp;		/* New mask record */


 /*
  * Range-check...
  */

  if (loc == NULL)
    return (NULL);

 /*
  * Try to allocate memory for the record...
  */

  if (loc->num_allow == 0)
    temp = malloc(sizeof(cupsd_authmask_t));
  else
    temp = realloc(loc->allow, sizeof(cupsd_authmask_t) * (loc->num_allow + 1));

  if (temp == NULL)
    return (NULL);

  loc->allow = temp;
  temp       += loc->num_allow;
  loc->num_allow ++;

 /*
  * Clear the mask record and return...
  */

  memset(temp, 0, sizeof(cupsd_authmask_t));
  return (temp);
}


/*
 * 'add_deny()' - Add a deny mask to the location.
 */

static cupsd_authmask_t *		/* O - New mask record */
add_deny(cupsd_location_t *loc)	/* I - Location to add to */
{
  cupsd_authmask_t	*temp;		/* New mask record */


 /*
  * Range-check...
  */

  if (loc == NULL)
    return (NULL);

 /*
  * Try to allocate memory for the record...
  */

  if (loc->num_deny == 0)
    temp = malloc(sizeof(cupsd_authmask_t));
  else
    temp = realloc(loc->deny, sizeof(cupsd_authmask_t) * (loc->num_deny + 1));

  if (temp == NULL)
    return (NULL);

  loc->deny = temp;
  temp      += loc->num_deny;
  loc->num_deny ++;

 /*
  * Clear the mask record and return...
  */

  memset(temp, 0, sizeof(cupsd_authmask_t));
  return (temp);
}


#if !HAVE_LIBPAM
/*
 * 'cups_crypt()' - Encrypt the password using the DES or MD5 algorithms,
 *                  as needed.
 */

static char *				/* O - Encrypted password */
cups_crypt(const char *pw,		/* I - Password string */
           const char *salt)		/* I - Salt (key) string */
{
  if (strncmp(salt, "$1$", 3) == 0)
  {
   /*
    * Use MD5 passwords without the benefit of PAM; this is for
    * Slackware Linux, and the algorithm was taken from the
    * old shadow-19990827/lib/md5crypt.c source code... :(
    */

    int			i;		/* Looping var */
    unsigned long	n;		/* Output number */
    int			pwlen;		/* Length of password string */
    const char		*salt_end;	/* End of "salt" data for MD5 */
    char		*ptr;		/* Pointer into result string */
    _cups_md5_state_t	state;		/* Primary MD5 state info */
    _cups_md5_state_t	state2;		/* Secondary MD5 state info */
    unsigned char	digest[16];	/* MD5 digest result */
    static char		result[120];	/* Final password string */


   /*
    * Get the salt data between dollar signs, e.g. $1$saltdata$md5.
    * Get a maximum of 8 characters of salt data after $1$...
    */

    for (salt_end = salt + 3; *salt_end && (salt_end - salt) < 11; salt_end ++)
      if (*salt_end == '$')
        break;

   /*
    * Compute the MD5 sum we need...
    */

    pwlen = strlen(pw);

    _cups_md5_init(&state);
    _cups_md5_append(&state, (unsigned char *)pw, pwlen);
    _cups_md5_append(&state, (unsigned char *)salt, salt_end - salt);

    _cups_md5_init(&state2);
    _cups_md5_append(&state2, (unsigned char *)pw, pwlen);
    _cups_md5_append(&state2, (unsigned char *)salt + 3, salt_end - salt - 3);
    _cups_md5_append(&state2, (unsigned char *)pw, pwlen);
    _cups_md5_finish(&state2, digest);

    for (i = pwlen; i > 0; i -= 16)
      _cups_md5_append(&state, digest, i > 16 ? 16 : i);

    for (i = pwlen; i > 0; i >>= 1)
      _cups_md5_append(&state, (unsigned char *)((i & 1) ? "" : pw), 1);

    _cups_md5_finish(&state, digest);

    for (i = 0; i < 1000; i ++)
    {
      _cups_md5_init(&state);

      if (i & 1)
        _cups_md5_append(&state, (unsigned char *)pw, pwlen);
      else
        _cups_md5_append(&state, digest, 16);

      if (i % 3)
        _cups_md5_append(&state, (unsigned char *)salt + 3, salt_end - salt - 3);

      if (i % 7)
        _cups_md5_append(&state, (unsigned char *)pw, pwlen);

      if (i & 1)
        _cups_md5_append(&state, digest, 16);
      else
        _cups_md5_append(&state, (unsigned char *)pw, pwlen);

      _cups_md5_finish(&state, digest);
    }

   /*
    * Copy the final sum to the result string and return...
    */

    memcpy(result, salt, salt_end - salt);
    ptr = result + (salt_end - salt);
    *ptr++ = '$';

    for (i = 0; i < 5; i ++, ptr += 4)
    {
      n = (((digest[i] << 8) | digest[i + 6]) << 8);

      if (i < 4)
        n |= digest[i + 12];
      else
        n |= digest[5];

      to64(ptr, n, 4);
    }

    to64(ptr, digest[11], 2);
    ptr += 2;
    *ptr = '\0';

    return (result);
  }
  else
  {
   /*
    * Use the standard crypt() function...
    */

    return (crypt(pw, salt));
  }
}
#endif /* !HAVE_LIBPAM */


#if HAVE_LIBPAM
/*
 * 'pam_func()' - PAM conversation function.
 */

static int					/* O - Success or failure */
pam_func(int                      num_msg,	/* I - Number of messages */
         const struct pam_message **msg,	/* I - Messages */
         struct pam_response      **resp,	/* O - Responses */
         void                     *appdata_ptr)	/* I - Pointer to connection */
{
  int			i;			/* Looping var */
  struct pam_response	*replies;		/* Replies */
  cupsd_client_t		*client;		/* Pointer client connection */


 /*
  * Allocate memory for the responses...
  */

  if ((replies = malloc(sizeof(struct pam_response) * num_msg)) == NULL)
    return (PAM_CONV_ERR);

 /*
  * Answer all of the messages...
  */

  DEBUG_printf(("pam_func: appdata_ptr = %p\n", appdata_ptr));

#ifdef __hpux
 /*
  * Apparently some versions of HP-UX 11 have a broken pam_unix security
  * module.  This is a workaround...
  */

  client = auth_client;
  (void)appdata_ptr;
#else
  client = (cupsd_client_t *)appdata_ptr;
#endif /* __hpux */

  for (i = 0; i < num_msg; i ++)
  {
    DEBUG_printf(("pam_func: Message = \"%s\"\n", msg[i]->msg));

    switch (msg[i]->msg_style)
    {
      case PAM_PROMPT_ECHO_ON:
          DEBUG_printf(("pam_func: PAM_PROMPT_ECHO_ON, returning \"%s\"...\n",
	                client->username));
          replies[i].resp_retcode = PAM_SUCCESS;
          replies[i].resp         = strdup(client->username);
          break;

      case PAM_PROMPT_ECHO_OFF:
          DEBUG_printf(("pam_func: PAM_PROMPT_ECHO_OFF, returning \"%s\"...\n",
	                client->password));
          replies[i].resp_retcode = PAM_SUCCESS;
          replies[i].resp         = strdup(client->password);
          break;

      case PAM_TEXT_INFO:
          DEBUG_puts("pam_func: PAM_TEXT_INFO...");
          replies[i].resp_retcode = PAM_SUCCESS;
          replies[i].resp         = NULL;
          break;

      case PAM_ERROR_MSG:
          DEBUG_puts("pam_func: PAM_ERROR_MSG...");
          replies[i].resp_retcode = PAM_SUCCESS;
          replies[i].resp         = NULL;
          break;

      default:
          DEBUG_printf(("pam_func: Unknown PAM message %d...\n",
	                msg[i]->msg_style));
          free(replies);
          return (PAM_CONV_ERR);
    }
  }

 /*
  * Return the responses back to PAM...
  */

  *resp = replies;

  return (PAM_SUCCESS);
}
#else


/*
 * 'to64()' - Base64-encode an integer value...
 */

static void
to64(char          *s,	/* O - Output string */
     unsigned long v,	/* I - Value to encode */
     int           n)	/* I - Number of digits */
{
  const char	*itoa64 = "./0123456789"
                          "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                          "abcdefghijklmnopqrstuvwxyz";


  for (; n > 0; n --, v >>= 6)
    *s++ = itoa64[v & 0x3f];
}
#endif /* HAVE_LIBPAM */


/*
 * End of "$Id$".
 */
