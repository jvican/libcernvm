/**
 * This file is part of CernVM Web API Plugin.
 *
 * CVMWebAPI is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CVMWebAPI is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with CVMWebAPI. If not, see <http://www.gnu.org/licenses/>.
 *
 * Developed by Ioannis Charalampidis 2013
 * Contact: <ioannis.charalampidis[at]cern.ch>
 */

#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <iostream>
#include <sstream>
#include <map>
#include <algorithm>

#include <CernVM/Config.h>
#include <CernVM/Hypervisor/Virtualbox/VBoxInstance.h>
#include <CernVM/Hypervisor.h>
#include <CernVM/Utilities.h>

using namespace std;

/** =========================================== **\
            Virtualbox Implementation
\** =========================================== **/

/**
 * Check integrity of the hypervisor
 */
bool VBoxInstance::validateIntegrity() { 
    CRASH_REPORT_BEGIN;

    // Check if the hypervisor reflection has gone away
    if (!vboxExists()) {

        // Mark us as invalid and return false
        reflectionValid = false;
        return false;

    } else {

        // Detect and update VirtualBox Version
        std::vector< std::string > out;
        std::string err;
        this->exec("--version", &out, &err, execConfig);

#ifdef __linux__
        vboxDrvKernelLoaded = true;
#endif

        // Check for common errors
        for (std::vector< std::string >::iterator sit = out.begin(); sit != out.end(); ++sit) {
            if (sit->find("WARNING") != std::string::npos) {
                CVMWA_LOG("warning", "Warning keyword in the hypervisor version!");

                // On linux, there is a solvable case, where 'vboxdrv'
                // kernel module is not loaded. 
                // This function just sets a flag. The actions are taken
#ifdef __linux__
                if (sit->find("vboxdrv kernel module is not loaded") != std::string::npos) {
                    vboxDrvKernelLoaded = false;
                } else {
#endif
                return false;
#ifdef __linux__
                }
#endif
            }
            if (sit->find("ERROR") != std::string::npos) {
                CVMWA_LOG("warning", "Error keyword in the hypervisor version!");
                return false;
            }
        }
        if (!err.empty()) {
            CVMWA_LOG("warning", "Error message in the hypervisor version!");
            return false;
        }

        // If we got some output, extract version numbers
        if (out.size() > 0)
            version.set( out[out.size()-1] );

        // Query system properties in order to find the 
        // location of the guest additions ISO
        this->hvGuestAdditions = "";
        if (this->exec("list systemproperties", &out, &err, execConfig) == 0) {
            map<string, string> data;

            // Parse system output
            parseLines( &out, &data, ":", " \t", 0, 1 );

            // Look for guest additions ISO
            if (data.find("Default Guest Additions ISO") != data.end()) {
                this->hvGuestAdditions = systemPath(data["Default Guest Additions ISO"]);
            }

        }

        // Reflection is valid
        reflectionValid = true;
        return true;
    }

    CRASH_REPORT_END;
}

/** 
 * Return virtual machine information
 */
map<const string, const string> VBoxInstance::getMachineInfo( std::string uuid, int timeout ) {
    CRASH_REPORT_BEGIN;
    vector<string> lines;
    map<const string, const string> dat;
    string err;
    
    // Local exec config
    SysExecConfig config(execConfig);
    config.timeout = timeout;

    // Perform property update
    int ans;
    NAMED_MUTEX_LOCK( uuid );
    ans = this->exec("showvminfo "+uuid, &lines, &err, config );
    NAMED_MUTEX_UNLOCK;
    if (ans != 0) {
        dat.insert(make_pair(":ERROR:", ntos<int>( ans )));
        return dat;
    }
    
    /* Tokenize response */
    return tokenize( &lines, ':' );
    CRASH_REPORT_END;
};

/**
 * Return all the properties of the guest
 */
map<string, string> VBoxInstance::getAllProperties( string uuid ) {
    CRASH_REPORT_BEGIN;
    map<string, string> ans;
    vector<string> lines;
    string errOut;

    // Get guest properties
    NAMED_MUTEX_LOCK( uuid );
    if (this->exec( "guestproperty enumerate "+uuid, &lines, &errOut, execConfig ) == 0) {
        for (vector<string>::iterator it = lines.begin(); it < lines.end(); ++it) {
            string line = *it;

            /* Find the anchor locations */
            size_t kBegin = line.find("Name: ");
            if (kBegin == string::npos) continue;
            size_t kEnd = line.find(", value:");
            if (kEnd == string::npos) continue;
            size_t vEnd = line.find(", timestamp:");
            if (vEnd == string::npos) continue;

            /* Get key */
            kBegin += 6;
            string vKey = line.substr( kBegin, kEnd - kBegin );

            /* Get value */
            size_t vBegin = kEnd + 9;
            string vValue = line.substr( vBegin, vEnd - vBegin );

            /* Store values */
            ans[vKey] = vValue;

        }
    }
    NAMED_MUTEX_UNLOCK;

    return ans;
    CRASH_REPORT_END;
}

/**
 * Load sessions if they are not yet loaded
 */
bool VBoxInstance::waitTillReady( DomainKeystore & keystore, const FiniteTaskPtr & pf, const UserInteractionPtr & ui ) {
    CRASH_REPORT_BEGIN;
    
    // Update progress
    if (pf) pf->setMax(3);

#ifdef __linux__
    // Check for problems in linux where vbox kernel driver is not loaded
    if (!vboxDrvKernelLoaded) {

        // Confirm action to be taken by the user
        if (ui) {

            // Confirm with the user
            if (ui->confirm(
                "Virtualbox kernel driver problem", 
                "It seems VirtualBox did not manage to install the kernel driver. Do you want to try and fix this? (It will require root privileges)") != UI_OK) {
                // Alert
                ui->alert(
                    "Virtualbox kernel driver problem",
                    "Try to run the following command and then try again:\n\nsudo /etc/init.d/vboxdrv setup"
                );
                // Send error
                if (pf) pf->fail("vboxdrv kernel module is not loaded");
                // Abort
                return false;
            }

        }

        // Do some more in-depth analysis of the linux platform
        LINUX_INFO linuxInfo;
        getLinuxInfo( &linuxInfo );
        if (linuxInfo.terminalCmdline.empty()) {
            // Alert
            ui->alert(
                "Could not fix the problem",
                "We could not open a terminal for you. Please run the following command and try again:\n\nsudo /etc/init.d/vboxdrv setup"
            );
            // Send error
            if (pf) pf->fail("Could not find a usable terminal emulator");
            // Abort
            return false;
        }

        // Try to prompt user
        string cmdline = linuxInfo.terminalCmdline + "\"sudo /etc/init.d/vboxdrv setup\"";
        res = system( cmdline.c_str() );
        if (res < 0) {
            // Alert
            ui->alert(
                "Could not fix the problem",
                "Unable to install virtualbox kernel driver. Please make sure you have your linux kernel headers installed and try again."
            );
            // Send error
            if (pf) pf->fail("Virtualbox driver installation failed");
            // Abort
            return false;
        }

        // Re-check integrity (And check if this made things worse)
        if (!validateIntegrity() || !vboxDrvKernelLoaded) {
            // Alert
            ui->alert(
                "Could not fix the problem",
                "Unable to install virtualbox kernel driver. Please try to uninstall and re-install Virtualbox manually!"
            );
            // Send error
            if (pf) pf->fail("Could not validate hypervisor integrity after install");
            // Abort
            return false;
        }

    }
#endif
    if (pf) pf->done("VirtualBox driver in place");

    // Session loading takes time, so instead of blocking the plugin
    // at creation time, use this mechanism to delay-load it when first accessed.
    if (!this->sessionLoaded) {

        // Create a progress feedback for the session loading
        FiniteTaskPtr pfLoading;
        if (pf) pfLoading = pf->begin<FiniteTask>("Loading sessions");

        // Load sessions
        this->loadSessions( pfLoading );
        this->sessionLoaded = true;

    } else {
        if (pf) pf->done("Sessions are loaded");
    }
    
    // By the way, check if we have the extension pack installed
    if (!this->hasExtPack()) {

        // Create a progress feedback instance for the installer
        FiniteTaskPtr pfInstall;
        if (pf) pfInstall = pf->begin<FiniteTask>("Installing extension pack");

        // Extension pack is released under PUEL license
        // require the user to confirm before continuing
        if (ui) {
            if (ui->confirmLicense("VirtualBox Personal Use and Evaluation License (PUEL)", VBOX_PUEL_LICENSE) != UI_OK) {
                // (User did not click OK)

                // Send error
                if (pf) pf->fail("User denied Oracle PUEL license");

                // Abort
                return false;
            }
        }

        // Start extension pack installation
        this->installExtPack(
                keystore,
                this->downloadProvider,
                pfInstall
            );

    } else {
        if (pf) pf->done("Extension pack is installed");
    }

    if (pf) pf->complete("Hypervisor is ready");

    /**
     * All's good!
     */
    return true;
    CRASH_REPORT_END;
}

/**
 * Return a property from the VirtualBox guest
 */
std::string VBoxInstance::getProperty( std::string uuid, std::string name ) {
    CRASH_REPORT_BEGIN;
    vector<string> lines;
    string value;
    string err;
    
    /* Invoke property query */
    int ans;
    NAMED_MUTEX_LOCK( uuid );
    ans = this->exec("guestproperty get "+uuid+" \""+name+"\"", &lines, &err, execConfig);
    NAMED_MUTEX_UNLOCK;
    if (ans != 0) return "";
    if (lines.empty()) return "";
    
    /* Process response */
    value = lines[0];
    if (value.substr(0,6).compare("Value:") == 0) {
        return value.substr(7);
    } else {
        return "";
    }
    
    CRASH_REPORT_END;
}

/**
 * Return Virtualbox sessions instead of classic
 */
HVSessionPtr VBoxInstance::allocateSession() {
    CRASH_REPORT_BEGIN;
    
    // Allocate a new GUID for this session
    std::string guid = newGUID();

    // Fetch a config object
    LocalConfigPtr cfg = LocalConfig::forRuntime( "vbsess-" + guid );
    cfg->set("uuid", guid);

    // Return new session instance
    VBoxSessionPtr session = boost::make_shared< VBoxSession >( cfg, this->shared_from_this() );
    
    // Store on session registry and return session object
    this->sessions[ guid ] = session;
    return static_cast<HVSessionPtr>(session);

    CRASH_REPORT_END;
}

/**
 * Load capabilities
 */
int VBoxInstance::getCapabilities ( HVINFO_CAPS * caps ) {
    CRASH_REPORT_BEGIN;
    map<string, string> data;
    vector<string> lines, parts;
    string err;
    int v;
    
    // List the CPUID information
    int ans;
    NAMED_MUTEX_LOCK("generic");
    ans = this->exec("list hostcpuids", &lines, &err, execConfig);
    NAMED_MUTEX_UNLOCK;
    if (ans != 0) return HVE_QUERY_ERROR;
    if (lines.empty()) return HVE_EXTERNAL_ERROR;
    
    // Process lines
    for (vector<string>::iterator i = lines.begin(); i != lines.end(); i++) {
        string line = *i;
        if (trimSplit( &line, &parts, " \t", " \t") == 0) continue;
        if (parts[0].compare("00000000") == 0) { // Leaf 0 -> Vendor
            v = hex_ston<int>( parts[2] ); // EBX
            caps->cpu.vendor[0] = v & 0xFF;
            caps->cpu.vendor[1] = (v & 0xFF00) >> 8;
            caps->cpu.vendor[2] = (v & 0xFF0000) >> 16;
            caps->cpu.vendor[3] = (v & 0xFF000000) >> 24;
            v = hex_ston<int>( parts[4] ); // EDX
            caps->cpu.vendor[4] = v & 0xFF;
            caps->cpu.vendor[5] = (v & 0xFF00) >> 8;
            caps->cpu.vendor[6] = (v & 0xFF0000) >> 16;
            caps->cpu.vendor[7] = (v & 0xFF000000) >> 24;
            v = hex_ston<int>( parts[3] ); // ECX
            caps->cpu.vendor[8] = v & 0xFF;
            caps->cpu.vendor[9] = (v & 0xFF00) >> 8;
            caps->cpu.vendor[10] = (v & 0xFF0000) >> 16;
            caps->cpu.vendor[11] = (v & 0xFF000000) >> 24;
            caps->cpu.vendor[12] = '\0';
            
        } else if (parts[0].compare("00000001") == 0) { // Leaf 1 -> Features
            caps->cpu.featuresA = hex_ston<int>( parts[3] ); // ECX
            caps->cpu.featuresB = hex_ston<int>( parts[4] ); // EDX
            v = hex_ston<int>( parts[1] ); // EAX
            caps->cpu.stepping = v & 0xF;
            caps->cpu.model = (v & 0xF0) >> 4;
            caps->cpu.family = (v & 0xF00) >> 8;
            caps->cpu.type = (v & 0x3000) >> 12;
            caps->cpu.exmodel = (v & 0xF0000) >> 16;
            caps->cpu.exfamily = (v & 0xFF00000) >> 20;
            
        } else if (parts[0].compare("80000001") == 0) { // Leaf 80000001 -> Extended features
            caps->cpu.featuresC = hex_ston<int>( parts[3] ); // ECX
            caps->cpu.featuresD = hex_ston<int>( parts[4] ); // EDX
            
        }
    }
    
    // Update flags
    caps->cpu.hasVM = false; // Needs MSR to detect
    caps->cpu.hasVT = 
        ( (caps->cpu.featuresA & 0x20) != 0 ) || // Intel 'vmx'
        ( (caps->cpu.featuresC & 0x2)  != 0 );   // AMD 'svm'
    caps->cpu.has64bit =
        ( (caps->cpu.featuresC & 0x20000000) != 0 ); // Long mode 'lm'
        
    // List the system properties
    NAMED_MUTEX_LOCK("generic");
    ans = this->exec("list systemproperties", &lines, &err, execConfig);
    NAMED_MUTEX_UNLOCK;
    if (ans != 0) return HVE_QUERY_ERROR;
    if (lines.empty()) return HVE_EXTERNAL_ERROR;

    // Default limits
    caps->max.cpus = 1;
    caps->max.memory = 1024;
    caps->max.disk = 2048;
    
    // Tokenize into the data map
    parseLines( &lines, &data, ":", " \t", 0, 1 );
    if (data.find("Maximum guest RAM size") != data.end()) 
        caps->max.memory = ston<int>(data["Maximum guest RAM size"]);
    if (data.find("Virtual disk limit (info)") != data.end()) 
        caps->max.disk = ston<long>(data["Virtual disk limit (info)"]) / 1024;
    if (data.find("Maximum guest CPU count") != data.end()) 
        caps->max.cpus = ston<int>(data["Maximum guest CPU count"]);
    
    // Ok!
    return HVE_OK;
    CRASH_REPORT_END;
};

/**
 * Get a list of mediums managed by VirtualBox
 */
std::vector< std::map< const std::string, const std::string > > VBoxInstance::getDiskList() {
    CRASH_REPORT_BEGIN;
    vector<string> lines;
    std::vector< std::map< const std::string, const std::string > > emptyMap;
    string err;

    // List the running VMs in the system
    int ans;
    NAMED_MUTEX_LOCK("generic");
    ans = this->exec("list hdds", &lines, &err, execConfig);
    NAMED_MUTEX_UNLOCK;
    if (ans != 0) return emptyMap;
    if (lines.empty()) return emptyMap;

    // Tokenize lists
    std::vector< std::map< const std::string, const std::string > > resMap = tokenizeList( &lines, ':' );
    return resMap;
    CRASH_REPORT_END;
}

/**
 * Parse VirtualBox Log file in order to get the launched process PID
 */
int __getPIDFromFile( std::string logPath ) {
    CRASH_REPORT_BEGIN;
    int pid = 0;

    /* Locate Logfile */
    string logFile = logPath + "/VBox.log";
    CVMWA_LOG("Debug", "Looking for PID in " << logFile );
    if (!file_exists(logFile)) return 0;

    /* Open input stream */
    ifstream fIn(logFile.c_str(), ifstream::in);
    
    /* Read as few bytes as possible */
    string inBufferLine;
    size_t iStart, iEnd, i1, i2;
    char inBuffer[1024];
    while (!fIn.eof()) {

        // Read line
        fIn.getline( inBuffer, 1024 );

        // Handle it via higher-level API
        inBufferLine.assign( inBuffer );
        if ((iStart = inBufferLine.find("Process ID:")) != string::npos) {

            // Pick the appropriate ending
            iEnd = inBufferLine.length();
            i1 = inBufferLine.find("\r");
            i2 = inBufferLine.find("\n");
            if (i1 < iEnd) iEnd=i1;
            if (i2 < iEnd) iEnd=i2;

            // Extract string
            inBufferLine = inBufferLine.substr( iStart+12, iEnd-iStart );

            // Convert to integer
            pid = ston<int>( inBufferLine );
            break;
        }
    }

    CVMWA_LOG("Debug", "PID extracted from file: " << pid );

    // Close and return PID
    fIn.close();
    return pid;
    CRASH_REPORT_END;
}

/**
 * Return a VirtualBox Session based on the VirtualBox UUID specified
 */
HVSessionPtr VBoxInstance::sessionByVBID ( const std::string& virtualBoxGUID ) {
    CRASH_REPORT_BEGIN;

    // Look for a session with the given GUID
    for (std::map< std::string,HVSessionPtr >::iterator i = this->sessions.begin(); i != this->sessions.end(); i++) {
        HVSessionPtr sess = (*i).second;
        if (sess->parameters->get("vboxid", "").compare( virtualBoxGUID ) == 0 ) {
            return sess;
        }
    }

    // Return an unitialized HVSessionPtr if nothing is found
    return HVSessionPtr();
    CRASH_REPORT_END;
}

HVSessionPtr VBoxInstance::sessionOpen ( const ParameterMapPtr& parameters, const FiniteTaskPtr & pf ) {
    CRASH_REPORT_BEGIN;

    // Call parent function to open session
    HVSessionPtr  sess = HVInstance::sessionOpen(parameters,pf);
    VBoxSessionPtr vbs = boost::static_pointer_cast<VBoxSession>( sess );

    // Set progress feedack object
    vbs->FSMUseProgress( pf, "Updating VM information" );

    // Open session
    vbs->open();

    // Return instance
    return vbs;

    CRASH_REPORT_END;
}

/**
 * Remove a session object indexed by it's reference
 */
void VBoxInstance::sessionDelete ( const HVSessionPtr& session ) {
    CRASH_REPORT_BEGIN;

    // Iterate over sessions
    for (std::map< std::string,HVSessionPtr >::iterator i = this->sessions.begin(); i != this->sessions.end(); i++) {
        string uuid = (*i).first;
        HVSessionPtr sess = (*i).second;

        // Session found
        if ( uuid.compare(session->uuid) == 0 ) {

            // Loook for the session object in the open sessions
            for (std::list< HVSessionPtr >::iterator jt = openSessions.begin(); jt != openSessions.end(); ++jt) {
                HVSessionPtr openSess = (*jt);
                // Check if the session has gone away
                if ( uuid.compare(openSess->uuid) == 0 ) {
                    // Remove from open sessions
                    openSessions.erase( jt );
                    // Let session know that it has gone away
                    boost::static_pointer_cast<VBoxSession>(sess)->hvNotifyDestroyed();
                    break;
                }
            }

            // Erase session from the sessions list
            this->sessions.erase( i );

            // Erase session file from disk
            ostringstream oss;
            oss << "vbsess-" << uuid;
            LocalConfig::forRuntime(oss.str())->clear();

            // Done
            return;
        }
    }

    CRASH_REPORT_END;
}

/**
 * Remove session from open sessions
 */
void VBoxInstance::sessionClose( const HVSessionPtr& session ) {
    CRASH_REPORT_BEGIN;

    // Check if there are many open sessions
    if (--session->instances > 0)
        return;

    // Abort any open session FSM
    session->abort();

    // Loook for the session object in the open sessions & remove it
    for (std::list< HVSessionPtr >::iterator jt = openSessions.begin(); jt != openSessions.end(); ++jt) {
        HVSessionPtr openSess = (*jt);
        // Check if the session has gone away
        if ( session->uuid.compare(openSess->uuid) == 0 ) {
            // Remove from open sessions
            openSessions.erase( jt );
            break;
        }
    }

    // If session is in SS_MISSING state, remove it
    if (session->local->getNum<int>( "state" ) == SS_MISSING) {
        sessionDelete( session );
    }

    CRASH_REPORT_END;
}

/**
 * Load session state from VirtualBox
 */
int VBoxInstance::loadSessions( const FiniteTaskPtr & pf ) {
    CRASH_REPORT_BEGIN;
    HVSessionPtr inst;
    vector<string> lines;
    map<const string, const string> diskinfo, vboxVms;
    string secret, kk, kv;
    string err;

    // Acquire a system-wide mutex for session update
    NAMED_MUTEX_LOCK("session-update");

    // Initialize progress feedback
    if (pf) {
        pf->setMax(4);
        pf->doing("Loading sessions from disk");
    }

    // Reset sessions array
    if (!sessions.empty())
        sessions.clear();

    // [1] Load session registry from the disk
    // =======================================
    std::vector< std::string > vbDiskSessions  = LocalConfig::runtime()->enumFiles("vbsess-");
    for (std::vector< std::string >::iterator it = vbDiskSessions.begin(); it != vbDiskSessions.end(); ++it) {
        std::string sessName = *it;
        CVMWA_LOG("Debug", "Importing session config " << sessName << " from disk");

        // Load session config
        LocalConfigPtr sessConfig = LocalConfig::forRuntime( sessName );
        if (!sessConfig->contains("name")) {
            CVMWA_LOG("Warning", "Missing 'name' in file " << sessName );
        } else if (!sessConfig->contains("uuid")) {
            CVMWA_LOG("Warning", "Missing 'uuid' in file " << sessName );
        } else {
            // Store session with the given UUID
            sessions[ sessConfig->get("uuid") ] = boost::make_shared< VBoxSession >( 
                sessConfig, this->shared_from_this() 
            );
        }

    }

    // List the running VMs in the system
    int ans;
    ans = this->exec("list vms", &lines, &err, execConfig);
    if (ans != 0) return HVE_QUERY_ERROR;

    // Forward progress
    if (pf) {
        pf->done("Sessions loaded");
        pf->doing("Loading sessions from hypervisor");
    }

    // [2] Collect the running VM info
    // ================================
    map<const string, const string> vms = tokenize( &lines, '{' );
    for (std::map<const string, const string>::iterator jt, it=vms.begin(); it!=vms.end(); ++it) {
        string name = (*it).first;
        string uuid = (*it).second;
        name = name.substr(1, name.length()-3);
        uuid = uuid.substr(0, uuid.length()-1);

        // Make sure it's not an inaccessible machine
        if (name.find("<inaccessible>") != string::npos) {
            CVMWA_LOG("Warning", "Found inaccessible VM " << uuid)
            continue;
        }

        // Store on map
        jt = vboxVms.find(uuid);
        if (jt!=vboxVms.end()) vboxVms.erase(jt);
        vboxVms.insert(make_pair(uuid, name));
    }

    // Forward progress
    if (pf) {
        pf->done("Sessions loaded");
        pf->doing("Cleaning-up expired sessions");
    }

    // [3] Remove the VMs that are not registered 
    //     in the hypervisor.
    // ===========================================
    for (std::map< std::string,HVSessionPtr >::iterator it = this->sessions.begin(); it != this->sessions.end(); ++it) {
        HVSessionPtr sess = (*it).second;

        // Check if the stored session does not correlate
        // to a session in VirtualBox -> It means it was 
        // destroyed externally.
        if (vboxVms.find(sess->parameters->get("vboxid")) == vboxVms.end()) {

            // Delete session
            sessionDelete( sess );

            // Quit if we have no sessions left
            if (this->sessions.size() == 0) break;

            // Rewind iterator
            it = this->sessions.begin();

        }

    }

    // Forward progress
    if (pf) {
        pf->done("Sessions cleaned-up");
        pf->doing("Releasing old open sessions");
    }

    // [4] Check if some of the currently open session 
    //     was lost.
    // ===========================================
    for (std::list< HVSessionPtr >::iterator it = openSessions.begin(); it != openSessions.end(); ++it) {
        HVSessionPtr sess = (*it);

        // Check if the session has gone away
        if (sessions.find(sess->uuid) == sessions.end()) {
   
            // Let session know that it has gone away
            boost::static_pointer_cast<VBoxSession>(sess)->hvNotifyDestroyed();

            // Remove it from open and rewind
            openSessions.erase( it );
            it = openSessions.begin();

            // Quit if we are done
            if (openSessions.size() == 0) break;

        }

    }

    // Notify progress
    if (pf) pf->done("Old open sessions released");

    return 0;
    NAMED_MUTEX_UNLOCK;
    CRASH_REPORT_END;
}

/**
 * Abort what's happening and prepare for shutdown
 */
void VBoxInstance::abort() {
    CRASH_REPORT_BEGIN;

    // Abort all open sessions
    for (std::list< HVSessionPtr >::iterator it = openSessions.begin(); it != openSessions.end(); ++it) {
        HVSessionPtr sess = (*it);
        sess->abort();
    }

    // Cleanup
    openSessions.clear();
    sessions.clear();

    CRASH_REPORT_END;
}

/**
 * Check if the hypervisor has the extension pack installed (used for the more advanced RDP)
 */
bool VBoxInstance::hasExtPack() {
    CRASH_REPORT_BEGIN;
    
    /**
     * Check for extension pack
     */
    vector<string> lines;
    string err;
    NAMED_MUTEX_LOCK("generic");
    this->exec("list extpacks", &lines, &err, execConfig);
    NAMED_MUTEX_UNLOCK;
    for (std::vector<std::string>::iterator l = lines.begin(); l != lines.end(); l++) {
        if (l->find("Oracle VM VirtualBox Extension Pack") != string::npos) {
            return true;
        }
    }

    // Not found
    return false;
    CRASH_REPORT_END;
}

/**
 * Install extension pack
 *
 * This function is used in combination with the installHypervisor function from Hypervisor class, but it can also be used
 * on it's own.
 *
 */
int VBoxInstance::installExtPack( DomainKeystore & keystore, const DownloadProviderPtr & downloadProvider, const FiniteTaskPtr & pf ) {
    CRASH_REPORT_BEGIN;
    string requestBuf;
    string checksum;
    string err;
    vector<string> lines;

    // Local exec config
    SysExecConfig config(execConfig);

    // Notify extension pack installation
    if (pf) {
        pf->setMax(5, false);
        pf->doing("Preparing for extension pack installation");
    }

    // If we already have an extension pack, complete
    if (hasExtPack()) {
        if (pf) pf->complete("Already installed");
        return HVE_ALREADY_EXISTS;
    }

    // Begin a download task
    VariableTaskPtr downloadPf;
    if (pf) downloadPf = pf->begin<VariableTask>("Downloading hypervisor configuration");
    
    /* Contact the information point */
    CVMWA_LOG( "Info", "Fetching data" );
    ParameterMapPtr data = boost::make_shared<ParameterMap>();
    int res = keystore.downloadHypervisorConfig( downloadProvider, data );
    if ( res != HVE_OK ) {
        if ((res == HVE_NOT_VALIDATED) || (res == HVE_NOT_TRUSTED)) {
            if (pf) pf->fail("Hypervisor configuration integrity check failed", res);
        } else {
            if (pf) pf->fail("Unable to fetch hypervisor configuration", res);
        }
        return res;
    }

    // Build version string (it will be something like "vbox-2.4.12")
    ostringstream oss;
    oss << "vbox-" << version.major << "." << version.minor << "." << version.build;

    CVMWA_LOG("INFO", "Ver string: '" << oss.str() << "' from '" << version.verString << "'");

    // Prepare name constants to be looked up on the configuration url
    string kExtpackUrl = oss.str()      + "-extpack";
    string kExtpackChecksum = oss.str() + "-extpackChecksum";
    string kExtpackExt = ".vbox-extpack";

    // Verify integrity of the data
    if (!data->contains( kExtpackUrl )) {
        CVMWA_LOG( "Error", "ERROR: No extensions package URL found" );
        if (pf) pf->fail("No extensions package URL found", HVE_EXTERNAL_ERROR);
        return HVE_EXTERNAL_ERROR;
    }
    if (!data->contains( kExtpackChecksum )) {
        CVMWA_LOG( "Error", "ERROR: No extensions package checksum found" );
        if (pf) pf->fail("No extensions package checksum found", HVE_EXTERNAL_ERROR);
        return HVE_EXTERNAL_ERROR;
    }

    // Begin download
    if (pf) downloadPf = pf->begin<VariableTask>("Downloading extension pack");

    // Download extension pack
    string tmpExtpackFile = getTmpDir() + "/" + getFilename( data->get(kExtpackUrl) );
    CVMWA_LOG( "Info", "Downloading " << data->get(kExtpackUrl) << " to " << tmpExtpackFile  );
    res = downloadProvider->downloadFile( data->get(kExtpackUrl), tmpExtpackFile, downloadPf );
    CVMWA_LOG( "Info", "    : Got " << res  );
    if ( res != HVE_OK ) {
        if (pf) pf->fail("Unable to download extension pack", res);
        return res;
    }
    
    // Validate checksum
    if (pf) pf->doing("Validating extension pack integrity");
    sha256_file( tmpExtpackFile, &checksum );
    CVMWA_LOG( "Info", "File checksum " << checksum << " <-> " << data->get(kExtpackChecksum)  );
    if (checksum.compare( data->get(kExtpackChecksum) ) != 0) {
        if (pf) pf->fail("Extension pack integrity was not validated", HVE_NOT_VALIDATED);
        return HVE_NOT_VALIDATED;
    }
    if (pf) pf->done("Extension pack integrity validated");

    // Install extpack on virtualbox
    if (pf) pf->doing("Installing extension pack");
    if (pf) pf->markLengthy(true);
    NAMED_MUTEX_LOCK("generic");
    res = this->exec( "extpack install \"" + tmpExtpackFile + "\"", NULL, &err, config.setGUI(true) );
    NAMED_MUTEX_UNLOCK;
    if (res != HVE_OK) {
        if (pf) pf->fail("Extension pack failed to install", HVE_EXTERNAL_ERROR);
        if (pf) pf->markLengthy(false);
        return HVE_EXTERNAL_ERROR;
    }
    if (pf) pf->markLengthy(false);
    if (pf) pf->done("Installed extension pack");

    // Cleanup
    if (pf) pf->doing("Cleaning-up");
    remove( tmpExtpackFile.c_str() );
    if (pf) pf->done("Cleaned-up");

    // Complete
    if (pf) pf->complete("Extension pack installed successfully");
    return HVE_OK;

    CRASH_REPORT_END;
}
