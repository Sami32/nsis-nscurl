
//? Marius Negrutiu (mailto:marius.negrutiu@protonmail.com) :: 2019/11/20
//? Make HTTP/S requests using libcurl

#include "main.h"
#include "curl.h"
#include <mbedtls/ssl.h>
#include <mbedtls/sha1.h>
#include "crypto.h"


typedef struct {
	CRITICAL_SECTION		csLock;
	CHAR					szVersion[64];
	CHAR					szUserAgent[128];
} CURL_GLOBALS;


CURL_GLOBALS g_Curl = {0};
#define DEFAULT_HEADERS_VIRTUAL_SIZE	CURL_MAX_HTTP_HEADER	/// 100KB
#define DEFAULT_UKNOWN_VIRTUAL_SIZE		1024 * 1024 * 200		/// 200MB
//x #define DEBUG_TRANSFER_SLOWDOWN		50						/// Delays during transfer


// ----------------------------------------------------------------------
// TODO: Fix upload resume/reconnect
// TODO: Client certificate (CURLOPT_SSLCERT, CURLOPT_PROXY_SSLCERT)
// TODO: Certificate revocation (CURLOPT_CRLFILE, CURLOPT_PROXY_CRLFILE)
// TODO: Secure Proxy (CURLOPT_PROXY_CAPATH, CURLOPT_PROXY_SSL_VERIFYHOST, CURLOPT_PROXY_SSL_VERIFYPEER)
// TODO: HPKP - HTTP public key pinning (CURLOPT_PINNEDPUBLICKEY, CURLOPT_PROXY_PINNEDPUBLICKEY)
// TODO: Time conditional request (CURLOPT_TIMECONDITION, CURLOPT_TIMEVALUE)
// TODO: DOH (CURLOPT_DOH_URL)
// TODO: Query SSL info (certificate chain, cypher, etc.)
// ----------------------------------------------------------------------

//+ CurlRequestSizes
void CurlRequestComputeNumbers( _In_ PCURL_REQUEST pReq, _Out_opt_ PULONG64 piSizeTotal, _Out_opt_ PULONG64 piSizeXferred, _Out_opt_ PSHORT piPercent, _Out_opt_ PBOOL pbDown )
{
	assert( pReq );

	// Uploading data goes first. Downloading server's response (remote content) goes second
	if (pReq->Runtime.iUlTotal == 0 && pReq->Runtime.iDlTotal == 0 && pReq->Runtime.iResumeFrom > 0) {
		// Connecting (phase 0)
		if (pbDown)
			*pbDown = TRUE;
		if (piSizeTotal)
			*piSizeTotal = 0;
		if (piSizeXferred)
			*piSizeXferred = pReq->Runtime.iResumeFrom;
		if (piPercent)
			*piPercent = -1;		/// Unknown size
	} else if (pReq->Runtime.iDlXferred > 0 || pReq->Runtime.iDlTotal > 0 || pReq->Runtime.iResumeFrom > 0) {
		// Downloading (phase 2)
		if (pbDown)
			*pbDown = TRUE;
		if (piSizeTotal)
			*piSizeTotal = pReq->Runtime.iDlTotal + pReq->Runtime.iResumeFrom;
		if (piSizeXferred)
			*piSizeXferred = pReq->Runtime.iDlXferred + pReq->Runtime.iResumeFrom;
		if (piPercent) {
			*piPercent = -1;		/// Unknown size
			if (pReq->Runtime.iDlTotal + pReq->Runtime.iResumeFrom > 0)
				*piPercent = (SHORT)(((pReq->Runtime.iDlXferred + pReq->Runtime.iResumeFrom) * 100) / (pReq->Runtime.iDlTotal + pReq->Runtime.iResumeFrom));
		}
	} else {
		// Uploading (phase 1)
		if (pbDown)
			*pbDown = FALSE;
		if (piSizeTotal)
			*piSizeTotal = pReq->Runtime.iUlTotal;
		if (piSizeXferred)
			*piSizeXferred = pReq->Runtime.iUlXferred;
		if (piPercent) {
			*piPercent = -1;		/// Unknown size
			if (pReq->Runtime.iUlTotal > 0)
				*piPercent = (SHORT)((pReq->Runtime.iUlXferred * 100) / pReq->Runtime.iUlTotal);
		}
	}
}


//+ CurlRequestFormatError
void CurlRequestFormatError( _In_ PCURL_REQUEST pReq, _In_opt_ LPTSTR pszError, _In_opt_ ULONG iErrorLen, _Out_opt_ PBOOLEAN pbSuccess, _Out_opt_ PULONG piErrCode )
{
	if (pszError)
		pszError[0] = 0;
	if (pbSuccess)
		*pbSuccess = TRUE;
	if (piErrCode)
		*piErrCode = 0;
	if (pReq) {
		if (pReq->Error.iWin32 != ERROR_SUCCESS) {
			// Win32 error code
			if (pbSuccess) *pbSuccess = FALSE;
			if (pszError)  _sntprintf( pszError, iErrorLen, _T( "0x%x \"%s\"" ), pReq->Error.iWin32, pReq->Error.pszWin32 );
			if (piErrCode) *piErrCode = pReq->Error.iWin32;
		} else if (pReq->Error.iCurl != CURLE_OK) {
			// CURL error
			if (pbSuccess) *pbSuccess = FALSE;
			if (pszError)  _sntprintf( pszError, iErrorLen, _T( "0x%x \"%hs\"" ), pReq->Error.iCurl, pReq->Error.pszCurl );
			if (piErrCode) *piErrCode = pReq->Error.iCurl;
		} else {
			// HTTP status
			if (piErrCode) *piErrCode = pReq->Error.iHttp;
			if ((pReq->Error.iHttp == 0) || (pReq->Error.iHttp >= 200 && pReq->Error.iHttp < 300)) {
				if (pszError)  _sntprintf( pszError, iErrorLen, _T( "OK" ) );
			} else {
				if (pbSuccess) *pbSuccess = FALSE;
				if (pszError) {
					// Some servers don't return the Reason-Phrase in their Status-Line (e.g. https://files.loseapp.com/file)
					if (!pReq->Error.pszHttp) {
						if (pReq->Error.iHttp >= 200 && pReq->Error.iHttp < 300) {
							pReq->Error.pszHttp = MyStrDup( eA2A, "OK" );
						} else if (pReq->Error.iHttp == 400) {
							pReq->Error.pszHttp = MyStrDup( eA2A, "Bad Request" );
						} else if (pReq->Error.iHttp == 401) {
							pReq->Error.pszHttp = MyStrDup( eA2A, "Unauthorized" );
						} else if (pReq->Error.iHttp == 403) {
							pReq->Error.pszHttp = MyStrDup( eA2A, "Forbidden" );
						} else if (pReq->Error.iHttp == 404) {
							pReq->Error.pszHttp = MyStrDup( eA2A, "Not Found" );
						} else if (pReq->Error.iHttp == 405) {
							pReq->Error.pszHttp = MyStrDup( eA2A, "Method Not Allowed" );
						} else if (pReq->Error.iHttp == 500) {
							pReq->Error.pszHttp = MyStrDup( eA2A, "Internal Server Error" );
						} else {
							pReq->Error.pszHttp = MyStrDup( eA2A, "Error" );
						}
					}
					_sntprintf( pszError, iErrorLen, _T( "%d \"%hs\"" ), pReq->Error.iHttp, pReq->Error.pszHttp );
				}
			}
		}
	}
}


// ____________________________________________________________________________________________________________________________________ //
//                                                                                                                                      //


//++ CurlInitialize
ULONG CurlInitialize()
{
	TRACE( _T( "%hs()\n" ), __FUNCTION__ );

	InitializeCriticalSection( &g_Curl.csLock );
	{
		// Plugin version
		// Default user agent
		TCHAR szBuf[MAX_PATH] = _T( "" ), szVer[MAX_PATH];
		GetModuleFileName( g_hInst, szBuf, ARRAYSIZE( szBuf ) );
		MyReadVersionString( szBuf, _T( "FileVersion" ), szVer, ARRAYSIZE( szVer ) );
		MyStrCopy( eT2A, g_Curl.szVersion, ARRAYSIZE( g_Curl.szVersion ), szVer );
		_snprintf( g_Curl.szUserAgent, ARRAYSIZE( g_Curl.szUserAgent ), "nscurl/%s", g_Curl.szVersion );
	}

	curl_global_init( CURL_GLOBAL_ALL );
	return ERROR_SUCCESS;
}


//++ CurlDestroy
void CurlDestroy()
{
	TRACE( _T( "%hs()\n" ), __FUNCTION__ );

	DeleteCriticalSection( &g_Curl.csLock );
	curl_global_cleanup();
}


//++ CurlExtractCacert
//?  Extract the built-in certificate storage to "$PLUGINSDIR\cacert.pem"
//?  The function does nothing if the file already exists
ULONG CurlExtractCacert()
{
	ULONG e = ERROR_SUCCESS;

	assert( g_hInst != NULL );
	assert( g_variables != NULL );

	{
		TCHAR szPem[MAX_PATH];
		_sntprintf( szPem, ARRAYSIZE( szPem ), _T( "%s\\cacert.pem" ), getuservariableEx( INST_PLUGINSDIR ) );
		if (!MyFileExists( szPem )) {

			EnterCriticalSection( &g_Curl.csLock );
			
			if (!MyFileExists( szPem )) 		/// Verify again
				e = MySaveResource( (HMODULE)g_hInst, _T( "cacert.pem" ), MAKEINTRESOURCE( 1 ), 1033, szPem );
			
			LeaveCriticalSection( &g_Curl.csLock );
		}
	}

	return e;
}


//++ CurlParseRequestParam
BOOL CurlParseRequestParam( _In_ ULONG iParamIndex, _In_ LPTSTR pszParam, _In_ int iParamMaxLen, _Out_ PCURL_REQUEST pReq )
{
	BOOL bRet = TRUE;
	assert( iParamMaxLen && pszParam && pReq );

	if (iParamIndex == 0) {
		//? Params[0] is always the HTTP method
		//x	assert(
		//x		lstrcmpi( pszParam, _T( "GET" ) ) == 0 ||
		//x		lstrcmpi( pszParam, _T( "POST" ) ) == 0 ||
		//x		lstrcmpi( pszParam, _T( "HEAD" ) ) == 0
		//x	);
		MyFree( pReq->pszMethod );
		pReq->pszMethod = MyStrDup( eT2A, pszParam );
	} else if (iParamIndex == 1) {
		//? Params[1] is always the URL
		MyFree( pReq->pszURL );
		pReq->pszURL = MyStrDup( eT2A, pszParam );
	} else if (iParamIndex == 2) {
		//? Params[2] is always the output file/memory
		MyFree( pReq->pszPath );
		if (lstrcmpi( pszParam, _T( "MEMORY" ) ) != 0)
			pReq->pszPath = MyStrDup( eT2T, pszParam );
	} else if (lstrcmpi( pszParam, _T( "/HEADER" ) ) == 0) {
		if (popstring( pszParam ) == NOERROR && *pszParam) {
			// The string may contain multiple headers delimited by \r\n
			LPTSTR psz1, psz2;
			LPSTR pszA;
			UNREFERENCED_PARAMETER( pszA );
			for (psz1 = pszParam; *psz1; ) {
				for (; (*psz1 == _T('\r')) || (*psz1 == _T('\n')); psz1++);			/// Skip \r\n
				for (psz2 = psz1; (*psz2 != _T('\0')) && (*psz2 != _T('\r')) && (*psz2 != _T('\n')); psz2++);		/// Find next \r\n\0
				if (psz2 > psz1) {
					if ((pszA = MyStrDupN( eT2A, psz1, (int)(psz2 - psz1) )) != NULL) {
						pReq->pOutHeaders = curl_slist_append( pReq->pOutHeaders, pszA );
						MyFree( pszA );
					}
				}
				psz1 = psz2;
			}
		}
	} else if (lstrcmpi( pszParam, _T( "/POSTVAR" ) ) == 0) {
		LPSTR pszFilename = NULL, pszType = NULL, pszName = NULL, pszData = NULL;
		IDATA Data;
		/// Extract optional parameters "filename=XXX" and "type=XXX"
		int e = NOERROR;
		while (e == NOERROR) {
			if ((e = popstring( pszParam )) == NOERROR) {
				if (CompareString( CP_ACP, NORM_IGNORECASE, pszParam, 9, _T( "filename=" ), -1 ) == CSTR_EQUAL) {
					pszFilename = MyStrDup( eT2A, pszParam + 9 );
				} else if (CompareString( CP_ACP, NORM_IGNORECASE, pszParam, 5, _T( "type=" ), -1 ) == CSTR_EQUAL) {
					pszType = MyStrDup( eT2A, pszParam + 5 );
				} else {
					break;
				}
			}
		}
		/// Extract mandatory parameters "name" and IDATA
		if (e == NOERROR) {
			pszName = MyStrDup( eT2A, pszParam );
			if ((e = popstring( pszParam )) == NOERROR) {
				if (IDataParseParam( pszParam, iParamMaxLen, &Data )) {

					// Store 5-tuple MIME form part
					pReq->pPostVars = curl_slist_append( pReq->pPostVars, pszFilename ? pszFilename : "" );
					pReq->pPostVars = curl_slist_append( pReq->pPostVars, pszType ? pszType : "" );
					pReq->pPostVars = curl_slist_append( pReq->pPostVars, pszName ? pszName : "" );
					{
						CHAR szType[] = {Data.Type, '\0'};
						pReq->pPostVars = curl_slist_append( pReq->pPostVars, szType );
					}
					{
						if (Data.Type == IDATA_TYPE_STRING) {
							pszData = MyStrDup( eA2A, Data.Str );
						} else if (Data.Type == IDATA_TYPE_FILE) {
							pszData = MyStrDup( eT2A, Data.File );
						} else if (Data.Type == IDATA_TYPE_MEM) {
							pszData = EncBase64( Data.Mem, (size_t)Data.Size );
						} else {
							assert( !"Unexpected IDATA type" );
						}
						pReq->pPostVars = curl_slist_append( pReq->pPostVars, pszData );
					}

					IDataDestroy( &Data );
				}
			}
		}
		MyFree( pszFilename );
		MyFree( pszType );
		MyFree( pszName );
		MyFree( pszData );
	} else if (lstrcmpi( pszParam, _T( "/PROXY" ) ) == 0) {
		if (popstring( pszParam ) == NOERROR && *pszParam) {
			MyFree( pReq->pszProxy );
			pReq->pszProxy = MyStrDup( eT2A, pszParam );
		}
	} else if (lstrcmpi( pszParam, _T( "/DATA" ) ) == 0) {
		if (popstring( pszParam ) == NOERROR && *pszParam)
			IDataParseParam( pszParam, iParamMaxLen, &pReq->Data );
	} else if (lstrcmpi( pszParam, _T( "/RESUME" ) ) == 0) {
		pReq->bResume = TRUE;
	} else if (lstrcmpi( pszParam, _T( "/CONNECTTIMEOUT" ) ) == 0 || lstrcmpi( pszParam, _T( "/TIMEOUT" ) ) == 0) {
		pReq->iConnectTimeout = popint();
	} else if (lstrcmpi( pszParam, _T( "/COMPLETETIMEOUT" ) ) == 0) {
		pReq->iCompleteTimeout = popint();
	} else if (lstrcmpi( pszParam, _T( "/REFERER" ) ) == 0 || lstrcmpi( pszParam, _T( "/REFERRER" ) ) == 0) {
		if (popstring( pszParam ) == NOERROR && *pszParam) {
			MyFree( pReq->pszReferrer );
			pReq->pszReferrer = MyStrDup( eT2A, pszParam );
		}
	} else if (lstrcmpi( pszParam, _T( "/USERAGENT" ) ) == 0) {
		if (popstring( pszParam ) == NOERROR && *pszParam) {
			MyFree( pReq->pszAgent );
			pReq->pszAgent = MyStrDup( eT2A, pszParam );
		}
	} else if (lstrcmpi( pszParam, _T( "/NOREDIRECT" ) ) == 0) {
		pReq->bNoRedirect = TRUE;
	} else if (lstrcmpi( pszParam, _T( "/AUTH" ) ) == 0) {
		int e;
		pReq->iAuthType = CURLAUTH_ANY;
		if ((e = popstring( pszParam )) == NOERROR) {
			if (CompareString( CP_ACP, NORM_IGNORECASE, pszParam, 5, _T( "type=" ), -1 ) == CSTR_EQUAL) {
				LPCTSTR pszType = pszParam + 5;
				if (lstrcmpi( pszType, _T( "basic" ) ) == 0)
					pReq->iAuthType = CURLAUTH_BASIC;
				else if (lstrcmpi( pszType, _T( "digest" ) ) == 0)
					pReq->iAuthType = CURLAUTH_DIGEST;
				else if (lstrcmpi( pszType, _T( "digestie" ) ) == 0)
					pReq->iAuthType = CURLAUTH_DIGEST_IE;
				else if (lstrcmpi( pszType, _T( "bearer" ) ) == 0)
					pReq->iAuthType = CURLAUTH_BEARER;
				e = popstring( pszParam );
			}
			if (e == NOERROR) {
				// TODO: Encrypt user/pass/token in memory
				if (pReq->iAuthType == CURLAUTH_BEARER) {
					pReq->pszPass = MyStrDup( eT2A, pszParam );		/// OAuth 2.0 token (stored as password)
				} else {
					pReq->pszUser = MyStrDup( eT2A, pszParam );
					if ((e = popstring( pszParam )) == NOERROR)
						pReq->pszPass = MyStrDup( eT2A, pszParam );
				}
			}
		}
	} else if (lstrcmpi( pszParam, _T( "/TLSAUTH" ) ) == 0) {
		if (popstring( pszParam ) == NOERROR) {
			// TODO: Encrypt user/pass/token in memory
			pReq->pszTlsUser = MyStrDup( eT2A, pszParam );
			if (popstring( pszParam ) == NOERROR)
				pReq->pszTlsPass = MyStrDup( eT2A, pszParam );
		}
	} else if (lstrcmpi( pszParam, _T( "/CACERT" ) ) == 0) {
		if (popstring( pszParam ) == NOERROR) {						/// pszParam may be empty ("")
			MyFree( pReq->pszCacert );
			pReq->pszCacert = MyStrDup( eT2A, pszParam );
		}
	} else if (lstrcmpi( pszParam, _T( "/CERT" ) ) == 0) {
		if (popstring( pszParam ) == NOERROR && *pszParam) {
			/// Validate SHA1 hash
			if (lstrlen( pszParam ) == 40) {
				int i;
				for (i = 0; isxdigit( pszParam[i] ); i++);
				if (i == 40) {
					LPSTR psz = MyStrDup( eT2A, pszParam );
					if (psz) {
						pReq->pCertList = curl_slist_append( pReq->pCertList, psz );
						MyFree( psz );
					}
				}
			}
		}
	} else if (lstrcmpi( pszParam, _T( "/DEBUG" ) ) == 0) {
		if (popstring( pszParam ) == NOERROR && *pszParam) {
			MyFree( pReq->pszDebugFile );
			pReq->pszDebugFile = MyStrDup( eT2T, pszParam );
		}
	} else {
		bRet = FALSE;	/// This parameter is not valid for Request
	}

	return bRet;
}



//++ MbedtlsCertVerify
int MbedtlsCertVerify(void *pParam, mbedtls_x509_crt *crt, int iChainIndex, uint32_t *piVerifyFlags)
{
	//? This function gets called to evaluate server's SSL certificates
	//? The calls are made sequentially for each certificate in the chain
	//? That usually happens three times per connection: #2(root certificate), #1(intermediate certificate), #0(user certificate)

	//? Our logic:
	//? * If all certificates in the chain are UNTRUSTED, we'll set the MBEDTLS_X509_BADCERT_NOT_TRUSTED verification flag when returning from the last call (index #0)
	//? * If we TRUST at least one certificate we simply do nothing (return all zeros)

	PCURL_REQUEST pReq = (PCURL_REQUEST)pParam;
	UCHAR Thumbprint[20];
	CHAR szThumbprint[41];

	assert( pReq->pCertList );

	if (!crt)
		return 1;

	// Remember the original root verification flags
	if (pReq->Runtime.iRootCertFlags == -1)
		pReq->Runtime.iRootCertFlags = *piVerifyFlags;

	// Reset verification flags for this certificate
	// Under the hood they are all merged (OR-ed) into a master value
	*piVerifyFlags = 0;

	// Compute certificate's thumbprint
	mbedtls_sha1_context sha1c;
	mbedtls_sha1_init( &sha1c );
	mbedtls_sha1_starts( &sha1c );
	mbedtls_sha1_update( &sha1c, crt->raw.p, crt->raw.len );
	mbedtls_sha1_finish( &sha1c, Thumbprint );
	mbedtls_sha1_free( &sha1c );
	MyFormatBinaryHexA( Thumbprint, sizeof( Thumbprint ), szThumbprint, sizeof( szThumbprint ) );

	// Verify against our trusted certificate list
	{
		struct curl_slist *p;
		for (p = pReq->pCertList; p; p = p->next) {
			if (lstrcmpiA( p->data, szThumbprint ) == 0) {

				pReq->Runtime.bTrustedCert = TRUE;
				break;
			}
		}
		TRACE( _T( "Certificate( #%d, \"%hs\" ) = %hs\n" ), iChainIndex, szThumbprint, p ? "TRUSTED" : "UNTRUSTED" );

		/// Print the certificate
	#if 0
	#if defined (TRACE_ENABLED)
		{
			CHAR szCert[2048];
			mbedtls_x509_crt_info( szCert, ARRAYSIZE( szCert ), "                ", crt );
			TRACE( TRACE_NO_PREFIX _T( "%hs" ), szCert );
		}
	#endif
	#endif
	}

	// Final verdict
	if (iChainIndex == 0) {
		if (pReq->Runtime.bTrustedCert) {
			// Clear all verification flags pre-calculated by mbedTLS
			// If the certificate thumbprint is explicitly trusted, we don't care about the status of the certificate (expired, revoked, whatever...)
			// The connection is allowed to continue
			*piVerifyFlags = 0;
		} else {
			// Return the original verification flags, pre-calculated by mbedTLS
			*piVerifyFlags = pReq->Runtime.iRootCertFlags;
		}
		TRACE( _T( "Certificate verification flags: 0x%x (original) -> 0x%x (final)\n" ), pReq->Runtime.iRootCertFlags, *piVerifyFlags );
	}

	return 0;	/// Success always
}


//++ CurlSSLCallback
//? This callback function gets called by libcurl just before the initialization of an SSL connection
CURLcode CurlSSLCallback( CURL *curl, void *ssl_ctx, void *userptr )
{
	PCURL_REQUEST pReq = (PCURL_REQUEST)userptr;
	mbedtls_ssl_config *pssl = (mbedtls_ssl_config*)ssl_ctx;

	assert( pReq && pReq->Runtime.pCurl == curl);
	assert( pReq->pCertList );

	//? Setup an mbedTLS low-level callback function to analyze server's certificate chain
	pssl->f_vrfy = MbedtlsCertVerify;
	pssl->p_vrfy = pReq;

	return CURLE_OK;
}


//++ CurlHeaderCallback
size_t CurlHeaderCallback( char *buffer, size_t size, size_t nitems, void *userdata )
{
	PCURL_REQUEST pReq = (PCURL_REQUEST)userdata;
	LPSTR psz1, psz2;

	assert( pReq && pReq->Runtime.pCurl );

	//x TRACE( _T( "%hs( \"%hs\" )\n" ), __FUNCTION__, buffer );

	// NOTE: This callback function receives incoming headers one at a time
	// Headers from multiple (redirected) connections are separated by an empty line ("\r\n")
	// We only want to keep the headers from the last connection
	if (pReq->Runtime.InHeaders.iSize == 0 ||	/// First connection
		(
		pReq->Runtime.InHeaders.iSize > 4 &&
		pReq->Runtime.InHeaders.pMem[pReq->Runtime.InHeaders.iSize - 4] == '\r' &&
		pReq->Runtime.InHeaders.pMem[pReq->Runtime.InHeaders.iSize - 3] == '\n' &&
		pReq->Runtime.InHeaders.pMem[pReq->Runtime.InHeaders.iSize - 2] == '\r' &&
		pReq->Runtime.InHeaders.pMem[pReq->Runtime.InHeaders.iSize - 1] == '\n')
		)
	{
		// The last received header is an empty line ("\r\n")
		// Discard existing headers and start collecting a new set
		VirtualMemoryReset( &pReq->Runtime.InHeaders );

		// Extract HTTP status text from header
		// e.g. "HTTP/1.1 200 OK" -> "OK"
		// e.g. "HTTP/1.1 404 NOT FOUND" -> "NOT FOUND"
		// e.g. "HTTP/1.1 200 " -> Some servers don't return the Reason-Phrase in their Status-Line (e.g. https://files.loseapp.com/file)
		MyFree( pReq->Error.pszHttp );
		for (psz1 = buffer; (*psz1 != ' ') && (*psz1 != '\0'); psz1++);		/// Find first whitespace
		if (*psz1++ == ' ') {
			for (; (*psz1 != ' ') && (*psz1 != '\0'); psz1++);				/// Find second whitespace
			if (*psz1++ == ' ') {
				for (psz2 = psz1; (*psz2 != '\r') && (*psz2 != '\n') && (*psz2 != '\0'); psz2++);	/// Find trailing \r\n
				if (psz2 > psz1)
					pReq->Error.pszHttp = MyStrDupN( eA2A, psz1, (int)(psz2 - psz1) );
			}
		}

		// Collect HTTP connection info
		/// Server IP address
		curl_easy_getinfo( pReq->Runtime.pCurl, CURLINFO_PRIMARY_IP, &psz1 );
		MyFree( pReq->Runtime.pszServerIP );
		pReq->Runtime.pszServerIP = MyStrDup( eA2A, psz1 );

		/// Server port
		curl_easy_getinfo( pReq->Runtime.pCurl, CURLINFO_PRIMARY_PORT, &pReq->Runtime.iServerPort );

		// Collect last effective URL
		MyFree( pReq->Runtime.pszFinalURL );
		curl_easy_getinfo( pReq->Runtime.pCurl, CURLINFO_EFFECTIVE_URL, &psz1 );
		pReq->Runtime.pszFinalURL = MyStrDup( eA2A, psz1 );
	}

	// Collect incoming headers
	return VirtualMemoryAppend( &pReq->Runtime.InHeaders, buffer, size * nitems );
}


//++ CurlReadCallback
size_t CurlReadCallback( char *buffer, size_t size, size_t nitems, void *instream )
{
	curl_off_t l = 0;
	PCURL_REQUEST pReq = (PCURL_REQUEST)instream;
	assert( pReq && pReq->Runtime.pCurl );

#ifdef DEBUG_TRANSFER_SLOWDOWN
	Sleep( DEBUG_TRANSFER_SLOWDOWN );
#endif

	if (pReq->Data.Type == IDATA_TYPE_STRING || pReq->Data.Type == IDATA_TYPE_MEM) {
		// Input string/memory buffer
		assert( pReq->Runtime.iDataPos <= pReq->Data.Size );
		l = __min( size * nitems, pReq->Data.Size - pReq->Runtime.iDataPos );
		CopyMemory( buffer, (PCCH)pReq->Data.Str + pReq->Runtime.iDataPos, (size_t)l );
		pReq->Runtime.iDataPos += l;
	} else if (pReq->Data.Type == IDATA_TYPE_FILE) {
		// Input file
		if (MyValidHandle( pReq->Runtime.hInFile )) {
			ULONG iRead;
			if (ReadFile( pReq->Runtime.hInFile, (LPVOID)buffer, size * nitems, &iRead, NULL )) {
				l = iRead;
				pReq->Runtime.iDataPos += iRead;
			} else {
				l = CURL_READFUNC_ABORT;
			}
		}
	} else {
		assert( !"Unexpected IDATA type" );
	}

	return (size_t)l;
}


//++ CurlWriteCallback
size_t CurlWriteCallback( char *ptr, size_t size, size_t nmemb, void *userdata )
{
	PCURL_REQUEST pReq = (PCURL_REQUEST)userdata;
	assert( pReq && pReq->Runtime.pCurl );

#ifdef DEBUG_TRANSFER_SLOWDOWN
	Sleep( DEBUG_TRANSFER_SLOWDOWN );
#endif

	if (MyValidHandle( pReq->Runtime.hOutFile )) {
		// Write to output file
		ULONG iWritten = 0;
		if (WriteFile( pReq->Runtime.hOutFile, ptr, (ULONG)(size * nmemb), &iWritten, NULL )) {
			return iWritten;
		} else {
			pReq->Error.iWin32 = GetLastError();
			pReq->Error.pszWin32 = MyFormatError( pReq->Error.iWin32 );
		}
	} else {
		// Initialize output virtual memory (once)
		if (!pReq->Runtime.OutData.pMem) {
			curl_off_t iMaxSize;
			if (curl_easy_getinfo( pReq->Runtime.pCurl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &iMaxSize ) != CURLE_OK)
				iMaxSize = DEFAULT_UKNOWN_VIRTUAL_SIZE;
			if (iMaxSize == -1)
				iMaxSize = DEFAULT_UKNOWN_VIRTUAL_SIZE;
			VirtualMemoryInitialize( &pReq->Runtime.OutData, (SIZE_T)iMaxSize );
		}
		// Write to RAM
		return VirtualMemoryAppend( &pReq->Runtime.OutData, ptr, size * nmemb );
	}

	return 0;
}


//++ CurlProgressCallback
int CurlProgressCallback( void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow )
{
	PCURL_REQUEST pReq = (PCURL_REQUEST)clientp;
	curl_off_t iTotal, iXferred, iSpeed;
	curl_off_t iTimeElapsed, iTimeRemaining = 0;

	assert( pReq && pReq->Runtime.pCurl );

	if (TermSignaled())
		return CURLE_ABORTED_BY_CALLBACK;

	if (InterlockedCompareExchange(&pReq->Queue.iFlagAbort, -1, -1) != FALSE)
		return CURLE_ABORTED_BY_CALLBACK;

//x	TRACE( _T( "%hs( DL:%I64u/%I64u, UL:%I64u/%I64u )\n" ), __FUNCTION__, dlnow, dltotal, ulnow, ultotal );

	curl_easy_getinfo( pReq->Runtime.pCurl, CURLINFO_TOTAL_TIME_T, &iTimeElapsed );
	if (dlnow > 0) {
		/// Downloading (phase 2)
		iTotal = dltotal, iXferred = dlnow;
		curl_easy_getinfo( pReq->Runtime.pCurl, CURLINFO_SPEED_DOWNLOAD_T, &iSpeed );
		iTimeRemaining = (dltotal * iTimeElapsed) / dlnow - iTimeElapsed;
	} else {
		/// Uploading (phase 1)
		iTotal = ultotal, iXferred = ulnow;
		curl_easy_getinfo( pReq->Runtime.pCurl, CURLINFO_SPEED_UPLOAD_T, &iSpeed );
		if (ulnow > 0)
			iTimeRemaining = (ultotal * iTimeElapsed) / ulnow - iTimeElapsed;
	}

	pReq->Runtime.iTimeElapsed = GetTickCount() - pReq->Runtime.iTimeStart;		/// Aggregated elapsed time
	pReq->Runtime.iTimeRemaining = iTimeRemaining / 1000;						/// us -> ms
	pReq->Runtime.iDlTotal = dltotal;
	pReq->Runtime.iDlXferred = dlnow;
	pReq->Runtime.iUlTotal = ultotal;
	pReq->Runtime.iUlXferred = ulnow;
	pReq->Runtime.iSpeed = iSpeed;
	MemoryBarrier();

	return CURLE_OK;
}


//++ CurlDebugCallback
int CurlDebugCallback( CURL *handle, curl_infotype type, char *data, size_t size, void *userptr )
{
	PCURL_REQUEST pReq = (PCURL_REQUEST)userptr;
	assert( pReq && pReq->Runtime.pCurl == handle );

	if (type == CURLINFO_HEADER_OUT) {

		// NOTE: This callback function receives outgoing headers all at once

		// A block of outgoing headers are sent for every (redirected) connection
		// We'll collect only the last block
		VirtualMemoryReset( &pReq->Runtime.OutHeaders );
		VirtualMemoryAppend( &pReq->Runtime.OutHeaders, data, size );

	} else if (type == CURLINFO_HEADER_IN) {

		// NOTE: This callback function receives incoming headers one at a time
		// NOTE: Incoming header are handled by CurlHeaderCallback(..)

	}

	// Debug file
	if (MyValidHandle( pReq->Runtime.hDebugFile )) {
	
		ULONG iBytes;
		LPSTR pszPrefix;
		const size_t iMaxLen = 1024 * 128;
		const size_t iLineLen = 512;
		LPSTR pszLine;

		// Prefix
		switch (type) {
			case CURLINFO_TEXT:			pszPrefix = "[-] "; break;
			case CURLINFO_HEADER_IN:	pszPrefix = "[<h] "; break;
			case CURLINFO_HEADER_OUT:	pszPrefix = "[>h] "; break;
			case CURLINFO_DATA_IN:		pszPrefix = "[<d] "; break;
			case CURLINFO_DATA_OUT:		pszPrefix = "[>d] "; break;
			case CURLINFO_SSL_DATA_IN:	pszPrefix = "[<s] "; break;
			case CURLINFO_SSL_DATA_OUT:	pszPrefix = "[>s] "; break;
			default:					pszPrefix = "[?] ";
		}
		WriteFile( pReq->Runtime.hDebugFile, pszPrefix, lstrlenA( pszPrefix ), &iBytes, NULL );

		// Data
		if ((pszLine = (LPSTR)malloc( iLineLen )) != NULL) {
			for (size_t iOutLen = 0; iOutLen < iMaxLen; ) {
				size_t i, n = __min( iLineLen, size - iOutLen );
				if (n == 0)
					break;
				for (i = 0; i < n; i++) {
					pszLine[i] = data[iOutLen + i];
					if (pszLine[i] == '\n') {
						i++;
						break;
					} else if ((pszLine[i] < 32 || pszLine[i] > 126) && pszLine[i] != '\r' && pszLine[i] != '\t') {
						pszLine[i] = '.';
					}
				}
				WriteFile( pReq->Runtime.hDebugFile, pszLine, (ULONG)i, &iBytes, NULL );
				if (i < 1 || pszLine[i - 1] != '\n')
					WriteFile( pReq->Runtime.hDebugFile, "\n", 1, &iBytes, NULL );
				iOutLen += i;
			}
			free( pszLine );
		}
	}

	return 0;
}


//++ CurlTransfer
void CurlTransfer( _In_ PCURL_REQUEST pReq )
{
	CURL *curl;
	curl_mime *form = NULL;
	CHAR szError[CURL_ERROR_SIZE] = "";		/// Runtime error buffer
	#define StringIsEmpty(psz)				((psz) != NULL && ((psz)[0] == 0))

	if (!pReq)
		return;
	if (!pReq->pszURL || !*pReq->pszURL) {
		pReq->Error.iWin32 = ERROR_INVALID_PARAMETER;
		pReq->Error.pszWin32 = MyFormatError( pReq->Error.iWin32 );
		return;
	}

	// Extract "$PLUGINSDIR\cacert.pem" once
	if (pReq->pszCacert == NULL)			/// pszCacert == NULL means embedded cacert.pem
		CurlExtractCacert();

	// Input file
	if (pReq->pszMethod && (
		lstrcmpiA( pReq->pszMethod, "PUT" ) == 0 ||
		(lstrcmpiA( pReq->pszMethod, "POST" ) == 0 && !pReq->pPostVars)
		))
	{
		if (pReq->Data.Type == IDATA_TYPE_FILE) {
			ULONG e = ERROR_SUCCESS;
			pReq->Runtime.hInFile = CreateFile( (LPCTSTR)pReq->Data.File, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL );
			if (MyValidHandle( pReq->Runtime.hInFile )) {
				// NOTE: kernel32!GetFileSizeEx is only available in XP+
				LARGE_INTEGER l;
				l.LowPart = GetFileSize( pReq->Runtime.hInFile, &l.HighPart );
				if (l.LowPart != INVALID_FILE_SIZE) {
					/// Store file size in iDataSize
					pReq->Data.Size = l.QuadPart;
				} else {
					e = GetLastError();
				}
			} else {
				e = GetLastError();
			}
			if (e != ERROR_SUCCESS && pReq->Error.iWin32 == ERROR_SUCCESS) {
				pReq->Error.iWin32 = e;
				pReq->Error.pszWin32 = MyFormatError( e );
				TRACE( _T( "[!] CreateFile( DataFile:%s ) = %s\n" ), (LPCTSTR)pReq->Data.File, pReq->Error.pszWin32 );
			}
		}
	}

	// Output file
	if (pReq->pszPath && *pReq->pszPath) {
		ULONG e = ERROR_SUCCESS;
		MyCreateDirectory( pReq->pszPath, TRUE );	// Create intermediate directories
		pReq->Runtime.hOutFile = CreateFile( pReq->pszPath, GENERIC_WRITE, FILE_SHARE_READ, NULL, (pReq->bResume ? OPEN_ALWAYS : CREATE_ALWAYS), FILE_ATTRIBUTE_NORMAL, NULL );
		if (MyValidHandle( pReq->Runtime.hOutFile )) {
			/// Resume?
			if (pReq->bResume) {
				LARGE_INTEGER l;
				l.LowPart = GetFileSize( pReq->Runtime.hOutFile, &l.HighPart );
				if (l.LowPart != INVALID_FILE_SIZE) {
					pReq->Runtime.iResumeFrom = l.QuadPart;
					if (SetFilePointer( pReq->Runtime.hOutFile, 0, NULL, FILE_END ) == INVALID_SET_FILE_POINTER) {
						e = GetLastError();
					}
				} else {
					e = GetLastError();
				}
			}
		} else {
			e = GetLastError();
		}
		if (e != ERROR_SUCCESS && pReq->Error.iWin32 == ERROR_SUCCESS) {
			pReq->Error.iWin32 = e;
			pReq->Error.pszWin32 = MyFormatError( e );
			TRACE( _T( "[!] CreateFile( OutFile:%s ) = %s\n" ), pReq->pszPath, pReq->Error.pszWin32 );
		}
	}

	// Debug file
	if (pReq->pszDebugFile && *pReq->pszDebugFile) {
		ULONG e = ERROR_SUCCESS;
		MyCreateDirectory( pReq->pszDebugFile, TRUE );		// Create intermediate directories
		pReq->Runtime.hDebugFile = CreateFile( pReq->pszDebugFile, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL );
		if (!MyValidHandle( pReq->Runtime.hOutFile )) {
			e = GetLastError();
			TRACE( _T( "[!] CreateFile( DebugFile:%s ) = 0x%x\n" ), pReq->pszDebugFile, e );
		}
	}

	if (pReq->Error.iWin32 == ERROR_SUCCESS) {

		// Transfer
		curl = curl_easy_init();	// TODO: Cache
		if (curl) {

			/// Remember it
			pReq->Runtime.pCurl = curl;
			pReq->Runtime.iTimeStart = GetTickCount();

			/// Error buffer
			szError[0] = ANSI_NULL;
			curl_easy_setopt( curl, CURLOPT_ERRORBUFFER, szError );

			curl_easy_setopt( curl, CURLOPT_USERAGENT, pReq->pszAgent ? pReq->pszAgent : g_Curl.szUserAgent );
			if (pReq->pszReferrer)
				curl_easy_setopt( curl, CURLOPT_REFERER, pReq->pszReferrer );

			if (pReq->bNoRedirect) {
				curl_easy_setopt( curl, CURLOPT_FOLLOWLOCATION, FALSE );
			} else {
				curl_easy_setopt( curl, CURLOPT_FOLLOWLOCATION, TRUE );			/// Follow redirects
				curl_easy_setopt( curl, CURLOPT_MAXREDIRS, 10 );
			}

			if (pReq->iConnectTimeout > 0)
				curl_easy_setopt( curl, CURLOPT_CONNECTTIMEOUT_MS, pReq->iConnectTimeout );
			if (pReq->iCompleteTimeout > 0)
				curl_easy_setopt( curl, CURLOPT_TIMEOUT_MS, pReq->iCompleteTimeout );

			/// SSL
			if (!StringIsEmpty(pReq->pszCacert) || pReq->pCertList) {
				// SSL validation enabled
				curl_easy_setopt( curl, CURLOPT_SSL_VERIFYPEER, TRUE );		/// Verify SSL certificate
				curl_easy_setopt( curl, CURLOPT_SSL_VERIFYHOST, 2 );		/// Validate host name
				// cacert.pem
				if (pReq->pszCacert == NULL) {
					CHAR szCacert[MAX_PATH];
				#if _UNICODE
					_snprintf( szCacert, ARRAYSIZE( szCacert ), "%ws\\cacert.pem", getuservariableEx( INST_PLUGINSDIR ) );
				#else
					_snprintf( szCacert, ARRAYSIZE( szCacert ), "%s\\cacert.pem", getuservariableEx( INST_PLUGINSDIR ) );
				#endif
					curl_easy_setopt( curl, CURLOPT_CAINFO, szCacert );		/// Embedded cacert.pem
					assert( MyFileExistsA( szCacert ) );
				} else if (!StringIsEmpty( pReq->pszCacert )) {
					curl_easy_setopt( curl, CURLOPT_CAINFO, pReq->pszCacert );	/// Custom cacert.pem
					assert( MyFileExistsA( pReq->pszCacert ) );
				} else {
					curl_easy_setopt( curl, CURLOPT_CAINFO, NULL );			/// No cacert.pem
				}
				// Additional trusted certificates
				if (pReq->pCertList) {
					// SSL callback
					curl_easy_setopt( curl, CURLOPT_SSL_CTX_FUNCTION, CurlSSLCallback );
					curl_easy_setopt( curl, CURLOPT_SSL_CTX_DATA, pReq );
				}
			} else {
				// SSL validation disabled
				curl_easy_setopt( curl, CURLOPT_SSL_VERIFYPEER, FALSE );
				curl_easy_setopt( curl, CURLOPT_SSL_VERIFYHOST, FALSE );
			}

			/// Request method
			if (!pReq->pszMethod || !*pReq->pszMethod ||
				lstrcmpiA( pReq->pszMethod, "GET" ) == 0)
			{

				// GET
				curl_easy_setopt( curl, CURLOPT_HTTPGET, TRUE );

			} else if (lstrcmpiA( pReq->pszMethod, "POST" ) == 0) {

				// POST
				curl_easy_setopt( curl, CURLOPT_POST, TRUE );
				if (pReq->pPostVars) {
					/// Send variables as multi-part MIME form (CURLOPT_MIMEPOST, "multipart/form-data")
					form = curl_mime_init( curl );
					if (form) {
						struct curl_slist *p;
						for (p = pReq->pPostVars; p; p = p->next) {
							curl_mimepart *part = curl_mime_addpart( form );
							if (part) {
								char iType;
								/// String 1
								if (p && p->data && *p->data)
									curl_mime_filename( part, p->data );
								/// String 2
								p = p->next;
								assert( p && p->data );
								if (p && p->data && *p->data)
									curl_mime_type( part, p->data );
								/// String 3
								p = p->next;
								assert( p && p->data );
								if (p && p->data && *p->data)
									curl_mime_name( part, p->data );
								/// String 4
								p = p->next;
								assert( p && p->data );
								iType = p->data[0];
								assert( iType == IDATA_TYPE_STRING || iType == IDATA_TYPE_FILE || iType == IDATA_TYPE_MEM );
								/// String 5
								p = p->next;
								assert( p && p->data );
								if (p && p->data && *p->data) {
									if (iType == IDATA_TYPE_STRING) {
										curl_mime_data( part, p->data, CURL_ZERO_TERMINATED );	/// Data string
									} else if (iType == IDATA_TYPE_FILE) {
										curl_mime_filedata( part, p->data );					/// Data file
									} else if (iType == IDATA_TYPE_MEM) {
										size_t ptrsize;
										PVOID ptr = DecBase64( p->data, &ptrsize );
										if (ptr)
											curl_mime_data( part, ptr, ptrsize );				/// Data buffer
										assert( ptr );
										MyFree( ptr );
									}
								}
							}
						}
					}
					curl_easy_setopt( curl, CURLOPT_MIMEPOST, form );
				} else {
					/// Send input data as raw form (CURLOPT_POSTFIELDS, "application/x-www-form-urlencoded")
					//! The caller is responsible to format/escape the input data
					curl_easy_setopt( curl, CURLOPT_POSTFIELDSIZE_LARGE, pReq->Data.Size );
				}

			} else if (lstrcmpiA( pReq->pszMethod, "HEAD" ) == 0) {

				// HEAD
				curl_easy_setopt( curl, CURLOPT_NOBODY, TRUE );

			} else if (lstrcmpiA( pReq->pszMethod, "PUT" ) == 0) {

				// PUT
				curl_easy_setopt( curl, CURLOPT_PUT, TRUE );
				curl_easy_setopt( curl, CURLOPT_INFILESIZE_LARGE, pReq->Data.Size );		/// "Content-Length: <filesize>" header is mandatory in HTTP/1.x

			} else {

				// DELETE, OPTIONS, TRACE, etc.
				curl_easy_setopt( curl, CURLOPT_CUSTOMREQUEST, pReq->pszMethod );
			}

			/// Request Headers
			if (pReq->pOutHeaders)
				curl_easy_setopt( curl, CURLOPT_HTTPHEADER, pReq->pOutHeaders );

			/// Proxy Server
			if (pReq->pszProxy)
				curl_easy_setopt( curl, CURLOPT_PROXY, pReq->pszProxy );

			/// TLS Authentication
			if (pReq->pszTlsUser && pReq->pszTlsPass) {
				curl_easy_setopt( curl, CURLOPT_TLSAUTH_TYPE, "SRP" );
				curl_easy_setopt( curl, CURLOPT_TLSAUTH_USERNAME, pReq->pszTlsUser );		// TODO: Store it encrypted
				curl_easy_setopt( curl, CURLOPT_TLSAUTH_PASSWORD, pReq->pszTlsPass );		// TODO: Store it encrypted
			}

			/// HTTP Authentication
			if (pReq->iAuthType == CURLAUTH_ANY || pReq->iAuthType == CURLAUTH_BASIC || pReq->iAuthType == CURLAUTH_DIGEST || pReq->iAuthType == CURLAUTH_DIGEST_IE) {
				curl_easy_setopt( curl, CURLOPT_HTTPAUTH, pReq->iAuthType );
				curl_easy_setopt( curl, CURLOPT_USERNAME, pReq->pszUser );		// TODO: Store it encrypted
				curl_easy_setopt( curl, CURLOPT_PASSWORD, pReq->pszPass );		// TODO: Store it encrypted
			} else if (pReq->iAuthType == CURLAUTH_BEARER) {
				curl_easy_setopt( curl, CURLOPT_HTTPAUTH, pReq->iAuthType );
				curl_easy_setopt( curl, CURLOPT_XOAUTH2_BEARER, pReq->pszPass );
			}

			/// Resume
			if (pReq->Runtime.iResumeFrom > 0)
				curl_easy_setopt( curl, CURLOPT_RESUME_FROM_LARGE, pReq->Runtime.iResumeFrom );

			/// Callbacks
			VirtualMemoryInitialize( &pReq->Runtime.InHeaders, DEFAULT_HEADERS_VIRTUAL_SIZE );
			VirtualMemoryInitialize( &pReq->Runtime.OutHeaders, DEFAULT_HEADERS_VIRTUAL_SIZE );
			curl_easy_setopt( curl, CURLOPT_HEADERFUNCTION, CurlHeaderCallback );
			curl_easy_setopt( curl, CURLOPT_HEADERDATA, pReq );
			curl_easy_setopt( curl, CURLOPT_READFUNCTION, CurlReadCallback );
			curl_easy_setopt( curl, CURLOPT_READDATA, pReq );
			curl_easy_setopt( curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback );
			curl_easy_setopt( curl, CURLOPT_WRITEDATA, pReq );
			curl_easy_setopt( curl, CURLOPT_XFERINFOFUNCTION, CurlProgressCallback );
			curl_easy_setopt( curl, CURLOPT_XFERINFODATA, pReq );
			curl_easy_setopt( curl, CURLOPT_NOPROGRESS, FALSE );	/// Activate progress callback function
			curl_easy_setopt( curl, CURLOPT_DEBUGFUNCTION, CurlDebugCallback );
			curl_easy_setopt( curl, CURLOPT_DEBUGDATA, pReq );
			curl_easy_setopt( curl, CURLOPT_VERBOSE, TRUE );		/// Activate debugging callback function

			/// URL
			curl_easy_setopt( curl, CURLOPT_URL, pReq->pszURL );

			// Transfer retries loop
			{
				BOOLEAN bSuccess;
				ULONG i, iConnectionTimeStart, e;
				for (i = 0, iConnectionTimeStart = GetTickCount(); ; i++) {

					pReq->Runtime.iTimeElapsed = GetTickCount() - pReq->Runtime.iTimeStart;			/// Refresh elapsed time
					if (i > 0) {
						// Request new TCP/IP connection
						curl_easy_setopt( curl, CURLOPT_FRESH_CONNECT, TRUE );
						// Timeouts
						if (pReq->iCompleteTimeout > 0) {
							curl_off_t to = pReq->iCompleteTimeout - pReq->Runtime.iTimeElapsed;	/// Remaining complete timeout
							to = __max( to, 1 );
							curl_easy_setopt( curl, CURLOPT_TIMEOUT_MS, to );
						}
						// Resume size
						pReq->Runtime.iResumeFrom += pReq->Runtime.iDlXferred;
						curl_easy_setopt( curl, CURLOPT_RESUME_FROM_LARGE, pReq->Runtime.iResumeFrom );
					}

					//+ Transfer
					/// This call returns when the transfer completes
					pReq->Error.iCurl = curl_easy_perform( curl );

					// Collect error
					MyFree( pReq->Error.pszCurl );
					pReq->Error.pszCurl = MyStrDup( eA2A, *szError ? szError : curl_easy_strerror( pReq->Error.iCurl ) );
					curl_easy_getinfo( curl, CURLINFO_RESPONSE_CODE, (PLONG)&pReq->Error.iHttp );	/// ...might not be available
					szError[0] = 0;

					// Finished?
					CurlRequestFormatError( pReq, NULL, 0, &bSuccess, &e );
					if (bSuccess || pReq->Error.iCurl == CURLE_ABORTED_BY_CALLBACK || pReq->Error.iCurl == CURLE_OPERATION_TIMEDOUT || pReq->Error.iWin32 == ERROR_CANCELLED)
						break;

					// Cancel?
					if (InterlockedCompareExchange( &pReq->Queue.iFlagAbort, -1, -1 ) != FALSE) {
						MyFree( pReq->Error.pszWin32 );
						pReq->Error.iWin32 = ERROR_CANCELLED;
						pReq->Error.pszWin32 = MyFormatError( pReq->Error.iWin32 );
						break;
					}

					// Timeout?
					pReq->Runtime.iTimeElapsed = GetTickCount() - pReq->Runtime.iTimeStart;		/// Refresh elapsed time
					if (pReq->Runtime.iUlXferred > 0 || pReq->Runtime.iDlXferred > 0)
						iConnectionTimeStart = GetTickCount();	/// The previous connection was successful. Some data has been transferred. Reset connection startup time

					if (pReq->iConnectTimeout == 0)
						break;					/// No retries
					if ((GetTickCount() >= iConnectionTimeStart + pReq->iConnectTimeout) ||							/// Enforce "Connect" timeout
						((pReq->iCompleteTimeout > 0) && (pReq->Runtime.iTimeElapsed >= pReq->iCompleteTimeout)))	/// Enforce "Complete" timeout
					{
						MyFree( pReq->Error.pszWin32 );
						pReq->Error.iWin32 = ERROR_TIMEOUT;
						pReq->Error.pszWin32 = MyFormatError( pReq->Error.iWin32 );
						break;
					}

					// Retry
					Sleep( 500 );
				}
			}

			// Finalize
			{
				CHAR chZero = 0;
				VirtualMemoryAppend( &pReq->Runtime.InHeaders, &chZero, 1 );
				VirtualMemoryAppend( &pReq->Runtime.OutHeaders, &chZero, 1 );
			}

			// Cleanup
			curl_easy_reset( curl );
			curl_easy_cleanup( curl );		// TODO: Return to cache
		}

		curl_mime_free( form );
	}

	// Cleanup
	if (MyValidHandle( pReq->Runtime.hInFile ))
		CloseHandle( pReq->Runtime.hInFile ), pReq->Runtime.hInFile = NULL;
	if (MyValidHandle( pReq->Runtime.hOutFile ))
		CloseHandle( pReq->Runtime.hOutFile ), pReq->Runtime.hOutFile = NULL;
	if (MyValidHandle( pReq->Runtime.hDebugFile ))
		CloseHandle( pReq->Runtime.hDebugFile ), pReq->Runtime.hDebugFile = NULL;
	#undef StringIsEmpty
}


//+ [internal] CurlQueryKeywordCallback
void CALLBACK CurlQueryKeywordCallback(_Inout_ LPTSTR pszKeyword, _In_ ULONG iMaxLen, _In_ PVOID pParam)
{
	// NOTE: pReq may be NULL
	PCURL_REQUEST pReq = (PCURL_REQUEST)pParam;
	assert( pszKeyword );

	if (lstrcmpi( pszKeyword, _T( "@PLUGINNAME@" ) ) == 0) {
		MyStrCopy( eT2T, pszKeyword, iMaxLen, PLUGINNAME );
	} else if (lstrcmpi( pszKeyword, _T( "@PLUGINVERSION@" ) ) == 0) {
		MyStrCopy( eA2T, pszKeyword, iMaxLen, g_Curl.szVersion );
	} else if (lstrcmpi( pszKeyword, _T( "@PLUGINAUTHOR@" ) ) == 0) {
		TCHAR szPath[MAX_PATH] = _T( "" );
		GetModuleFileName( g_hInst, szPath, ARRAYSIZE( szPath ) );
		MyReadVersionString( szPath, _T( "CompanyName" ), pszKeyword, iMaxLen );
	} else if (lstrcmpi( pszKeyword, _T( "@PLUGINWEB@" ) ) == 0) {
		TCHAR szPath[MAX_PATH] = _T( "" );
		GetModuleFileName( g_hInst, szPath, ARRAYSIZE( szPath ) );
		MyReadVersionString( szPath, _T( "LegalTrademarks" ), pszKeyword, iMaxLen );
	} else if (lstrcmpi( pszKeyword, _T( "@CURLVERSION@" ) ) == 0) {
		//? e.g. "7.68.0"
		curl_version_info_data *ver = curl_version_info( CURLVERSION_NOW );
		MyStrCopy( eA2T, pszKeyword, iMaxLen, ver->version );
	} else if (lstrcmpi( pszKeyword, _T( "@CURLSSLVERSION@" ) ) == 0) {
		//? e.g. "mbedTLS/2.20.0"
		curl_version_info_data *ver = curl_version_info( CURLVERSION_NOW );
		MyStrCopy( eA2T, pszKeyword, iMaxLen, ver->ssl_version );
	} else if (lstrcmpi( pszKeyword, _T( "@CURLPROTOCOLS@" ) ) == 0) {
		curl_version_info_data *ver = curl_version_info( CURLVERSION_NOW );
		ULONG i, len;
		for (i = 0, len = 0, pszKeyword[0] = 0; ver->protocols && ver->protocols[i]; i++) {
			if (pszKeyword[0])
				pszKeyword[len++] = _T( ' ' ), pszKeyword[len] = _T( '\0' );
			MyStrCopy( eA2T, pszKeyword + len, iMaxLen - len, ver->protocols[i] );
			len += lstrlen( pszKeyword + len );
		}
	} else if (lstrcmpi( pszKeyword, _T( "@CURLFEATURES@" ) ) == 0) {
		curl_version_info_data *ver = curl_version_info( CURLVERSION_NOW );
		ULONG i, len;
		CHAR features[][16] = {
		"IPv6", "Kerberos4", "SSL", "libz", "NTLM", "GSSNEGOTIATE", "Debug", "AsynchDNS",
		"SPNEGO", "Largefile", "IDN", "SSPI", "CharConv", "TrackMemory", "TLS-SRP", "NTLM_WB",
		"HTTP2", "GSS-API", "Kerberos", "UnixSockets", "PSL", "HTTPS-proxy", "MultiSSL", "brotli",
		"alt-svc", "HTTP3", "ESNI"
		};
		for (i = 0, len = 0, pszKeyword[0] = 0; i < ARRAYSIZE( features ); i++) {
			if (ver->features & (1 << i)) {
				if (pszKeyword[0])
					pszKeyword[len++] = _T( ' ' ), pszKeyword[len] = _T( '\0' );
				MyStrCopy( eA2T, pszKeyword + len, iMaxLen - len, features[i] );
				len += lstrlen( pszKeyword + len );
			}
		}
	} else if (lstrcmpi( pszKeyword, _T( "@USERAGENT@" ) ) == 0) {
		MyStrCopy( eA2T, pszKeyword, iMaxLen, g_Curl.szUserAgent );
	} else if (pReq) {

		if (lstrcmpi( pszKeyword, _T( "@ID@" ) ) == 0) {
			_sntprintf( pszKeyword, iMaxLen, _T( "%u" ), pReq->Queue.iId );
		} else if (lstrcmpi( pszKeyword, _T( "@STATUS@" ) ) == 0) {
			switch (pReq->Queue.iStatus) {
				case STATUS_WAITING:  lstrcpyn( pszKeyword, _T( "Waiting" ), iMaxLen ); break;
				case STATUS_RUNNING:  lstrcpyn( pszKeyword, _T( "Running" ), iMaxLen ); break;
				case STATUS_COMPLETE: lstrcpyn( pszKeyword, _T( "Complete" ), iMaxLen ); break;
				default: assert( !"Unexpected request status" );
			}
		} else if (lstrcmpi( pszKeyword, _T( "@METHOD@" ) ) == 0) {
			MyStrCopy( eA2T, pszKeyword, iMaxLen, pReq->pszMethod ? pReq->pszMethod : "GET" );
		} else if (lstrcmpi( pszKeyword, _T( "@URL@" ) ) == 0) {
			MyStrCopy( eA2T, pszKeyword, iMaxLen, pReq->pszURL );
		} else if (lstrcmpi( pszKeyword, _T( "@FINALURL@" ) ) == 0) {
			MyStrCopy( eA2T, pszKeyword, iMaxLen, pReq->Runtime.pszFinalURL ? pReq->Runtime.pszFinalURL : "" );
		} else if (lstrcmpi( pszKeyword, _T( "@OUT@" ) ) == 0) {
			MyStrCopy( eT2T, pszKeyword, iMaxLen, pReq->pszPath ? pReq->pszPath : _T( "Memory" ) );
		} else if (lstrcmpi( pszKeyword, _T( "@OUTFILE@" ) ) == 0) {
			if (pReq->pszPath) {
				LPCTSTR psz, pszLastSep = NULL;
				for (psz = pReq->pszPath; *psz; psz++)
					if (*psz == '\\')
						pszLastSep = psz;		/// Last '\\'
				if (pszLastSep) {
					MyStrCopy( eT2T, pszKeyword, iMaxLen, pszLastSep + 1 );
				} else {
					MyStrCopy( eT2T, pszKeyword, iMaxLen, pReq->pszPath );
				}
			} else {
				MyStrCopy( eT2T, pszKeyword, iMaxLen, _T( "Memory" ) );
			}
		} else if (lstrcmpi( pszKeyword, _T( "@OUTDIR@" ) ) == 0) {
			if (pReq->pszPath) {
				LPCTSTR psz, pszLastSep = NULL;
				for (psz = pReq->pszPath; *psz; psz++)
					if (*psz == '\\')
						pszLastSep = psz;		/// Last '\\'
				if (pszLastSep) {
					for (; pszLastSep > pReq->pszPath && *pszLastSep == '\\'; pszLastSep--);	/// Move before '\\'
					lstrcpyn( pszKeyword, pReq->pszPath, (int)__min( iMaxLen, (ULONG)(pszLastSep - pReq->pszPath) + 2 ) );
				} else {
					MyStrCopy( eT2T, pszKeyword, iMaxLen, pReq->pszPath );
				}
			} else {
				MyStrCopy( eT2T, pszKeyword, iMaxLen, _T( "Memory" ) );
			}
		} else if (lstrcmpi( pszKeyword, _T( "@SERVERIP@" ) ) == 0) {
			MyStrCopy( eA2T, pszKeyword, iMaxLen, pReq->Runtime.pszServerIP );
		} else if (lstrcmpi( pszKeyword, _T( "@SERVERPORT@" ) ) == 0) {
			_sntprintf( pszKeyword, iMaxLen, _T( "%d" ), pReq->Runtime.iServerPort );
		} else if (lstrcmpi( pszKeyword, _T( "@FILESIZE@" ) ) == 0) {
			ULONG64 iTotal;
			CurlRequestComputeNumbers( pReq, &iTotal, NULL, NULL, NULL );
			MyFormatBytes( iTotal, pszKeyword, iMaxLen );
		} else if (lstrcmpi( pszKeyword, _T( "@FILESIZE_B@" ) ) == 0) {
			ULONG64 iTotal;
			CurlRequestComputeNumbers( pReq, &iTotal, NULL, NULL, NULL );
			_sntprintf( pszKeyword, iMaxLen, _T( "%I64u" ), iTotal );
		} else if (lstrcmpi( pszKeyword, _T( "@XFERSIZE@" ) ) == 0) {
			ULONG64 iXferred;
			CurlRequestComputeNumbers( pReq, NULL, &iXferred, NULL, NULL );
			MyFormatBytes( iXferred, pszKeyword, iMaxLen );
		} else if (lstrcmpi( pszKeyword, _T( "@XFERSIZE_B@" ) ) == 0) {
			ULONG64 iXferred;
			CurlRequestComputeNumbers( pReq, NULL, &iXferred, NULL, NULL );
			_sntprintf( pszKeyword, iMaxLen, _T( "%I64u" ), iXferred );
		} else if (lstrcmpi( pszKeyword, _T( "@PERCENT@" ) ) == 0) {
			SHORT iPercent;
			CurlRequestComputeNumbers( pReq, NULL, NULL, &iPercent, NULL );
			_sntprintf( pszKeyword, iMaxLen, _T( "%hd" ), iPercent );	/// Can be -1
		} else if (lstrcmpi( pszKeyword, _T( "@SPEED@" ) ) == 0) {
			MyFormatBytes( pReq->Runtime.iSpeed, pszKeyword, iMaxLen );
			_tcscat( pszKeyword, _T( "/s" ) );
		} else if (lstrcmpi( pszKeyword, _T( "@SPEED_B@" ) ) == 0) {
			_sntprintf( pszKeyword, iMaxLen, _T( "%I64u" ), pReq->Runtime.iSpeed );
		} else if (lstrcmpi( pszKeyword, _T( "@TIMEELAPSED@" ) ) == 0) {
			MyFormatMilliseconds( pReq->Runtime.iTimeElapsed, pszKeyword, iMaxLen );
		} else if (lstrcmpi( pszKeyword, _T( "@TIMEELAPSED_MS@" ) ) == 0) {
			_sntprintf( pszKeyword, iMaxLen, _T( "%I64u" ), pReq->Runtime.iTimeElapsed );
		} else if (lstrcmpi( pszKeyword, _T( "@TIMEREMAINING@" ) ) == 0) {
			MyFormatMilliseconds( pReq->Runtime.iTimeRemaining, pszKeyword, iMaxLen );
		} else if (lstrcmpi( pszKeyword, _T( "@TIMEREMAINING_MS@" ) ) == 0) {
			_sntprintf( pszKeyword, iMaxLen, _T( "%I64u" ), pReq->Runtime.iTimeRemaining );
		} else if (lstrcmpi( pszKeyword, _T( "@SENTHEADERS@" ) ) == 0) {
			int i;
			if (pReq->Runtime.OutHeaders.iSize) {
				MyStrCopy( eA2T, pszKeyword, iMaxLen, pReq->Runtime.OutHeaders.pMem );
			} else {
				pszKeyword[0] = 0;
			}
			for (i = lstrlen( pszKeyword ) - 1; (i >= 0) && (pszKeyword[i] == _T( '\r' ) || pszKeyword[i] == _T( '\n' )); i--)
				pszKeyword[i] = _T( '\0' );
			MyStrReplace( pszKeyword, iMaxLen, _T( "\r" ), _T( "\\r" ), FALSE );
			MyStrReplace( pszKeyword, iMaxLen, _T( "\n" ), _T( "\\n" ), FALSE );
			MyStrReplace( pszKeyword, iMaxLen, _T( "\t" ), _T( "\\t" ), FALSE );
		} else if (lstrcmpi( pszKeyword, _T( "@SENTHEADERS_RAW@" ) ) == 0) {
			if (pReq->Runtime.OutHeaders.iSize) {
				MyStrCopy( eA2T, pszKeyword, iMaxLen, pReq->Runtime.OutHeaders.pMem );
			} else {
				pszKeyword[0] = 0;
			}
		} else if (lstrcmpi( pszKeyword, _T( "@RECVHEADERS@" ) ) == 0) {
			int i;
			if (pReq->Runtime.InHeaders.iSize) {
				MyStrCopy( eA2T, pszKeyword, iMaxLen, pReq->Runtime.InHeaders.pMem );
			} else {
				pszKeyword[0] = 0;
			}
			for (i = lstrlen( pszKeyword ) - 1; (i >= 0) && (pszKeyword[i] == _T( '\r' ) || pszKeyword[i] == _T( '\n' )); i--)
				pszKeyword[i] = _T( '\0' );
			MyStrReplace( pszKeyword, iMaxLen, _T( "\r" ), _T( "\\r" ), FALSE );
			MyStrReplace( pszKeyword, iMaxLen, _T( "\n" ), _T( "\\n" ), FALSE );
			MyStrReplace( pszKeyword, iMaxLen, _T( "\t" ), _T( "\\t" ), FALSE );
		} else if (lstrcmpi( pszKeyword, _T( "@RECVHEADERS_RAW@" ) ) == 0) {
			if (pReq->Runtime.InHeaders.iSize) {
				MyStrCopy( eA2T, pszKeyword, iMaxLen, pReq->Runtime.InHeaders.pMem );
			} else {
				pszKeyword[0] = 0;
			}
		} else if (lstrcmpi( pszKeyword, _T( "@RECVDATA@" ) ) == 0 || lstrcmpi( pszKeyword, _T( "@RECVDATA_RAW@" ) ) == 0) {
			BOOL bEscape = (lstrcmpi( pszKeyword, _T( "@RECVDATA@" ) ) == 0) ? TRUE : FALSE;
			pszKeyword[0] = 0;
			if (pReq->pszPath) {
				// Downloaded to file
				LPSTR buf = (LPSTR)MyAlloc( iMaxLen );
				if (buf) {
					HANDLE h = CreateFile( pReq->pszPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL );
					if (MyValidHandle( h )) {
						ULONG l;
						if (ReadFile( h, buf, iMaxLen, &l, NULL ))
							MyFormatBinaryPrintable( buf, l, pszKeyword, iMaxLen, bEscape );
						CloseHandle( h );
					}
					MyFree( buf );
				}
			} else {
				// Downloaded to memory
				MyFormatBinaryPrintable( pReq->Runtime.OutData.pMem, pReq->Runtime.OutData.iSize, pszKeyword, iMaxLen, bEscape );
			}
		} else if (lstrcmpi( pszKeyword, _T( "@ERROR@" ) ) == 0) {
			CurlRequestFormatError( pReq, pszKeyword, iMaxLen, NULL, NULL );
		} else if (lstrcmpi( pszKeyword, _T( "@ERRORCODE@" ) ) == 0) {
			ULONG e;
			CurlRequestFormatError( pReq, NULL, 0, NULL, &e );
			_sntprintf( pszKeyword, iMaxLen, _T( "%u" ), e );
		} else if (lstrcmpi( pszKeyword, _T( "@CANCELLED@" ) ) == 0) {
			if (pReq->Error.iWin32 == ERROR_CANCELLED || pReq->Error.iCurl == CURLE_ABORTED_BY_CALLBACK) {
				lstrcpyn( pszKeyword, _T( "1" ), iMaxLen );
			} else {
				lstrcpyn( pszKeyword, _T( "0" ), iMaxLen );
			}
		}
	} else {
		// TODO: pReq is NULL. Replace all keywords with "", "n/a", etc.
	}
}


//++ CurlQuery
LONG CurlQuery( _In_ PCURL_REQUEST pReq, _Inout_ LPTSTR pszStr, _In_ LONG iStrMaxLen )
{
	if (!pszStr || !iStrMaxLen)
		return -1;

	return MyReplaceKeywords( pszStr, iStrMaxLen, _T( '@' ), _T( '@' ), CurlQueryKeywordCallback, pReq );
}
