import { OSDsPageHelper } from './osds.po';

describe('OSDs page', () => {
  const osds = new OSDsPageHelper();

  beforeEach(() => {
    cy.login();
    Cypress.Cookies.preserveOnce('token');
    osds.navigateTo();
  });

  describe('breadcrumb and tab tests', () => {
    it('should open and show breadcrumb', () => {
      osds.expectBreadcrumbText('OSDs');
    });

    it('should show two tabs', () => {
      osds.getTabsCount().should('eq', 2);
      osds.getTabText(0).should('eq', 'OSDs List');
      osds.getTabText(1).should('eq', 'Overall Performance');
    });
  });

  describe('check existence of fields on OSD page', () => {
    it('should check that number of rows and count in footer match', () => {
      osds.getTableTotalCount().then((text) => {
        osds.getTableRows().its('length').should('equal', text);
      });
    });

    it('should verify that buttons exist', () => {
      cy.contains('button', 'Create');
      cy.contains('button', 'Cluster-wide configuration');
    });

    describe('by selecting one row in OSDs List', () => {
      beforeEach(() => {
        osds.getExpandCollapseElement().click();
      });

      it('should show the correct text for the tab labels', () => {
        cy.get('#tabset-osd-details > div > tab').then(($tabs) => {
          const tabHeadings = $tabs.map((_i, e) => e.getAttribute('heading')).get();

          expect(tabHeadings).to.eql([
            'Devices',
            'Attributes (OSD map)',
            'Metadata',
            'Device health',
            'Performance counter',
            'Histogram',
            'Performance Details'
          ]);
        });
      });
    });
  });
});
